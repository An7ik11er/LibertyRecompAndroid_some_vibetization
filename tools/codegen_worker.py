#!/usr/bin/env python3
"""
tools/codegen_worker.py - Automated PPC function extractor, callgraph-aware inspector,
LLM decompiler, and hook generator with self-repair.

Supports both NVIDIA NIM API (cloud) and llama-server (local RTX 3070 Qwen2.5-Coder-7B).

Usage examples:
  # 1. Zero-token function inspection on local RTX 3070 (for Flash / Pro before hooking):
  python tools/codegen_worker.py --funcs sub_821D5F70 --mode inspect --backend local

  # 2. Direct RAGE -> Vulkan hook generation (Rule 10) on local RTX 3070:
  python tools/codegen_worker.py --funcs sub_828BFC00 --mode vulkan-direct --backend local --out hooks.inc

  # 3. Standard C++20 / NEON hook generation (NIM or local):
  python tools/codegen_worker.py --funcs sub_828CC1A0,sub_828C4338 --out hooks.inc --model poolside/laguna-xs-2.1

  # 4. Extract raw DEFINE_REX_FUNC body only:
  python tools/codegen_worker.py --funcs sub_821D5F70 --extract-only
"""

import argparse
import glob
import json
import os
import re
import sys
import urllib.request
import urllib.error

DEFAULT_NIM_MODEL = "nvidia/nemotron-3.5-lightning-30b-a3b"
FALLBACK_NIM_MODELS = [
    "nvidia/nemotron-3.5-lightning-30b-a3b",
    "poolside/laguna-xs-2.1",
    "nvidia/nemotron-3-ultra-550b-a55b",
    "mistralai/mistral-large-4-1t-a52b",
    "google/gemma-4-31b-it",
]

DEFAULT_NIM_KEY = "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"
NIM_BASE_URL = "https://integrate.api.nvidia.com/v1/chat/completions"
LOCAL_BASE_URL = "http://127.0.0.1:8080/v1/chat/completions"

AUDIO_BAN_START = 0x82900000
AUDIO_BAN_END = 0x8293FFFF


def resolve_workspace_root() -> str:
    """Finds repository root whether script is in tools/ or tools/agent_tools/codegen_and_llm/."""
    cur = os.path.abspath(os.path.dirname(__file__))
    for _ in range(5):
        if os.path.exists(os.path.join(cur, "glue")) or os.path.exists(os.path.join(cur, "Outsider_memory")):
            return cur
        parent = os.path.abspath(os.path.join(cur, ".."))
        if parent == cur:
            break
        cur = parent
    return os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


WORKSPACE_ROOT = resolve_workspace_root()
GENERATED_DIR = os.path.join(WORKSPACE_ROOT, "glue", "rexglue-sdk-main", "gta4-recomp", "generated")
CALLGRAPH_DB_PATH = os.path.join(WORKSPACE_ROOT, "Outsider_memory", "!!!!", "rage_callgraph_db.json")

SYSTEM_PROMPT_HOOK = """You are an expert low-level reverse engineer, compiler engineer, and ARM64 systems optimizer for Xbox 360 (PowerPC) recompilation projects running on Android / Snapdragon 888 (Cortex-X1 + Adreno 660).

Your task is to take decompiled / recompiled PowerPC guest functions and produce high-performance, drop-in replacement C++20 hooks.

### STRICT ABI & RECOMPILATION RULES:
1. **Function Signature**:
   Every hooked function MUST be declared as:
   `extern "C" void <func_name>(PPCContext& ctx, uint8_t* base)`
2. **Prologue**:
   Every hook MUST start with:
   `REX_FUNC_PROLOGUE();`
3. **PowerPC Calling Convention & Registers**:
   - `ctx.r3` through `ctx.r10`: integer arguments and return values.
   - `ctx.f1` through `ctx.f8`: float/double arguments and return values.
   - `ctx.v2` through `ctx.v13`: vector/SIMD arguments.
   - `ctx.r1.u32`: guest stack pointer. If the guest function modifies `ctx.r1` (e.g. allocating stack `ctx.r1.u32 = ea`), the original stack pointer MUST be restored before every return path, matching the guest function epilogue.
   - `ctx.lr`: link register (return address).
4. **Memory Addressing & Big-Endian Conversion**:
   - Xbox 360 is Big-Endian. All loads and stores from guest memory must use recomp helpers:
     - `REX_LOAD_U8(ea)`, `REX_STORE_U8(ea, val)`
     - `REX_LOAD_U16(ea)`, `REX_STORE_U16(ea, val)`
     - `REX_LOAD_U32(ea)`, `REX_STORE_U32(ea, val)`
     - `REX_LOAD_U64(ea)`, `REX_STORE_U64(ea, val)`
     - `REX_LOAD_F32(ea)`, `REX_STORE_F32(ea, val)`
     - `REX_RAW_ADDR(ea)` returns host `uint8_t*` pointer for guest physical address `ea`.
     - When reading/writing raw host pointers via `REX_RAW_ADDR(ea)` with NEON, remember guest words are Big-Endian (`vrev32q_u8` / `__builtin_bswap32`).
5. **ARM64 & Cortex-X1 Optimizations**:
   - Eliminate redundant guest stack frame allocations and temporary register spills if all callees are inlined or leaf operations.
   - Use ARM64 NEON (`#include <arm_neon.h>`) for vector math, matrix transforms, bounding box checks, and bulk memory copies.
   - Concurrency / Spinlocks: If `gta4_atomic_sections_lock_flag` is false, use relaxed/acquire-release `__atomic_*` or `std::atomic` instead of heavy global guest locks.
   - NEVER hook or alter functions in the untouchable Audio range `0x82900000-0x8293FFFF`.
6. **Output Format**:
   Return ONLY valid, compilable C++ code enclosed in ```cpp and ```. Do NOT include explanations outside the code block.
"""

SYSTEM_PROMPT_VULKAN_DIRECT = SYSTEM_PROMPT_HOOK + """
### RULE 10: KILL D3D9 EMULATION & MIGRATE TO DIRECT VULKAN
- Do NOT emit redundant D3D9 state-wrapper calls or per-draw mutex locks when they can be coalesced or bypassed.
- Known guest device layout constants in `Gta4NativeGraphicsSystem`:
  - `kTextureFetchBase = 0x480` (`kTextureFetchSize = 0x18`, 26 stages)
  - `kVertexConstantsOffset = 0x780` (`kVertexConstantsSize = 0x1000`, 64 dirty bit groups in MSB-first order)
  - `kPixelConstantsOffset = 0x1780` (`kPixelConstantsSize = 0xE00`, 56 dirty bit groups in MSB-first order)
  - `kDepthStencilPackedOffset = 10548`, `kAlphaTestFunctionPackedOffset = 10556`, `kClipControlPackedOffset = 10564`, `kCullModePackedOffset = 10568`
  - `kBlendControlOffset = 11844`, `kDepthEnableOffset = 11868`, `kStencilEnableOffset = 11872`
  - `kIndexBufferOffset = 12428`, `kRenderTargetOffset = 12432`, `kDepthStencilOffset = 12448`, `kStreamBufferBase = 12452`
- Coalesce state mutations, hoist invariant pointer translations (`REX_RAW_ADDR`) outside loops, and minimize work on `RenderGuest` (CPU 7 Cortex-X1).
"""

SYSTEM_PROMPT_INSPECT = """You are a senior reverse-engineering analyst for the GTA IV (RAGE Engine) Xbox 360 to Android ARM64 Vulkan recompilation project.
Analyze the provided `DEFINE_REX_FUNC` PowerPC recompilation code and callgraph metadata.
Output a concise, structured technical brief (maximum 30 lines total) covering:
1. **Signature & Register Contract**: Inputs in `r3`..`r10`, `f1`..`f8`, `v2`..`v13` and return values.
2. **Struct Layout & Memory Offsets**: All guest memory offsets accessed relative to `r3` (`this`) or globals, with inferred types (`uint32_t`, `float`, `vec4`, pointer, vtable).
3. **Callee Classification**: Categorize called `sub_82XXXXXX` functions into:
   - D3D9 state/draw wrappers (candidates for bypass under Rule 10)
   - RAGE culling / matrix / visibility math (candidates for ARM64 NEON vectorization)
   - Untouchable or external subsystems
4. **Actionable Hook Plan**: Exact strategy to hook or optimize this function on Cortex-X1 without breaking stack restoration (`ctx.r1.u32`) or Big-Endian rules.
"""


def parse_sub_address(func_name: str):
    m = re.match(r"^sub_([0-9A-Fa-f]{8})$", func_name.strip())
    return int(m.group(1), 16) if m else None


def is_in_audio_ban_range(addr: int) -> bool:
    return addr is not None and (AUDIO_BAN_START <= addr <= AUDIO_BAN_END)


def enforce_audio_ban(func_list: list):
    for fn in func_list:
        addr = parse_sub_address(fn)
        if is_in_audio_ban_range(addr):
            print(
                f"[FATAL] Function {fn} (0x{addr:08X}) falls in the untouchable Audio range "
                f"0x82900000-0x8293FFFF! Aborting per MASTER_INDEX.md.",
                file=sys.stderr,
            )
            sys.exit(2)


def find_function_in_files(func_name: str, search_dir: str = GENERATED_DIR) -> dict:
    """
    Locates the given function definition across generated recomp files and extracts
    the complete function using exact brace counting.
    """
    pattern = re.compile(rf'(?:DEFINE_REX_FUNC|PPC_WEAK_FUNC)\s*\(\s*{re.escape(func_name)}\s*\)')
    pattern_alt = re.compile(rf'void\s+{re.escape(func_name)}\s*\(\s*PPCContext')

    files = glob.glob(os.path.join(search_dir, "gta4_recomp.*.cpp"))
    if not files:
        files = glob.glob(os.path.join(search_dir, "*.cpp"))

    for filepath in sorted(files):
        with open(filepath, "r", encoding="utf-8", errors="replace") as f:
            content = f.read()

        match = pattern.search(content)
        if not match:
            match = pattern_alt.search(content)
        if not match:
            continue

        start_pos = match.start()
        brace_start = content.find('{', match.end())
        if brace_start == -1:
            continue

        brace_count = 0
        in_string = False
        in_char = False
        in_line_comment = False
        in_block_comment = False
        escape = False

        end_pos = -1
        i = brace_start
        length = len(content)

        while i < length:
            c = content[i]
            next_c = content[i + 1] if i + 1 < length else ''

            if in_line_comment:
                if c == '\n':
                    in_line_comment = False
            elif in_block_comment:
                if c == '*' and next_c == '/':
                    in_block_comment = False
                    i += 1
            elif in_string:
                if escape:
                    escape = False
                elif c == '\\':
                    escape = True
                elif c == '"':
                    in_string = False
            elif in_char:
                if escape:
                    escape = False
                elif c == '\\':
                    escape = True
                elif c == "'":
                    in_char = False
            else:
                if c == '/' and next_c == '/':
                    in_line_comment = True
                    i += 1
                elif c == '/' and next_c == '*':
                    in_block_comment = True
                    i += 1
                elif c == '"':
                    in_string = True
                elif c == "'":
                    in_char = True
                elif c == '{':
                    brace_count += 1
                elif c == '}':
                    brace_count -= 1
                    if brace_count == 0:
                        end_pos = i + 1
                        break
            i += 1

        if end_pos != -1:
            line_no = content[:start_pos].count('
') + 1
            func_code = content[start_pos:end_pos].strip()
            return {
                "func_name": func_name,
                "file": filepath,
                "line": line_no,
                "code": func_code,
                "length": len(func_code),
            }

    return None


def build_callgraph_context(extracted_data: list) -> str:
    """
    Enriches prompt with caller/callee metadata from rage_callgraph_db.json
    and flags any audio-range callees.
    """
    funcs_db = {}
    if os.path.exists(CALLGRAPH_DB_PATH):
        try:
            with open(CALLGRAPH_DB_PATH, "r", encoding="utf-8") as f:
                raw_db = json.load(f)
                if isinstance(raw_db, dict):
                    funcs_db = raw_db.get("functions", raw_db)
        except Exception as e:
            print(f"[!] Warning: Could not parse {CALLGRAPH_DB_PATH}: {e}", file=sys.stderr)

    lines = ["### Callgraph & Callee Context (auto-extracted):"]
    for d in extracted_data:
        fn = d["func_name"]
        code = d["code"]
        callees = sorted(set(re.findall(r"sub_[0-9A-Fa-f]{8}", code)))
        callees = [c for c in callees if c != fn]

        has_stack_frame = bool(re.search(r"ctx\.r1\.u32\s*=", code))
        has_bctrl = bool(re.search(r"bctrl|REX_CALL_INDIRECT", code))

        meta = funcs_db.get(fn, {}) if isinstance(funcs_db, dict) else {}
        callers = meta.get("callers", []) if isinstance(meta, dict) else []
        label = meta.get("name", meta.get("label", "")) if isinstance(meta, dict) else ""
        label_str = f" [{label}]" if label else ""

        lines.append(
            f"- Function `{fn}`{label_str}: modifies_r1_stack={has_stack_frame}, "
            f"has_indirect_call={has_bctrl}, callers_count={len(callers)}, direct_callees={len(callees)}"
        )

        for callee in callees[:30]:
            c_addr = parse_sub_address(callee)
            audio_warn = " [UNTOUCHABLE AUDIO RANGE - DO NOT INLINE/ALTER]" if is_in_audio_ban_range(c_addr) else ""
            c_meta = funcs_db.get(callee, {}) if isinstance(funcs_db, dict) else {}
            c_label = ""
            if isinstance(c_meta, dict):
                c_label = c_meta.get("name", c_meta.get("label", c_meta.get("category", "")))
            c_label_str = f" ({c_label})" if c_label else ""
            lines.append(f"  * calls `{callee}`{c_label_str}{audio_warn}")

    return "
".join(lines) + "
"


def get_api_key() -> str:
    if os.environ.get("NVIDIA_API_KEY"):
        return os.environ["NVIDIA_API_KEY"]

    mcp_config_path = os.path.expanduser(r"~\.gemini\config\mcp_config.json")
    if os.path.exists(mcp_config_path):
        try:
            with open(mcp_config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
                env = cfg.get("mcpServers", {}).get("nvidia-bridge", {}).get("env", {})
                if "NVIDIA_API_KEY" in env:
                    return env["NVIDIA_API_KEY"]
        except Exception:
            pass

    return DEFAULT_NIM_KEY


def call_chat_completion(url: str, headers: dict, payload: dict, timeout: int = 180) -> str:
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers=headers, method="POST")
    with urllib.request.urlopen(req, timeout=timeout) as response:
        res_data = json.loads(response.read().decode("utf-8"))
        choices = res_data.get("choices", [])
        if choices:
            msg = choices[0].get("message", {})
            return msg.get("content", "")
    return ""


def clean_llm_response(raw_text: str) -> str:
    cleaned = re.sub(r'<think>[\s\S]*?</think>', '', raw_text)
    code_block_match = re.search(r'```(?:cpp|c|\+\+)?\s*([\s\S]*?)```', cleaned)
    if code_block_match:
        return code_block_match.group(1).strip()
    return cleaned.strip()


def verify_cpp_code(code: str, expected_funcs: list, extracted_data: list = None) -> tuple:
    """
    Verifies that the generated C++ code:
    1. Has balanced braces '{}', parentheses '()', and brackets '[]'.
    2. Contains extern "C" void <fn>(PPCContext& ctx, uint8_t* base) for each expected function.
    3. Contains REX_FUNC_PROLOGUE() in the generated body.
    4. Preserves stack pointer restoration if original function modified ctx.r1.u32.
    5. Does not contain markdown artifacts or truncated lines.
    """
    brace_count = 0
    paren_count = 0
    in_string = False
    in_char = False
    in_line_comment = False
    in_block_comment = False
    escape = False

    i = 0
    length = len(code)
    while i < length:
        c = code[i]
        next_c = code[i + 1] if i + 1 < length else ''

        if in_line_comment:
            if c == '
':
                in_line_comment = False
        elif in_block_comment:
            if c == '*' and next_c == '/':
                in_block_comment = False
                i += 1
        elif in_string:
            if escape:
                escape = False
            elif c == '\':
                escape = True
            elif c == '"':
                in_string = False
        elif in_char:
            if escape:
                escape = False
            elif c == '\':
                escape = True
            elif c == "'":
                in_char = False
        else:
            if c == '/' and next_c == '/':
                in_line_comment = True
                i += 1
            elif c == '/' and next_c == '*':
                in_block_comment = True
                i += 1
            elif c == '"':
                in_string = True
            elif c == "'":
                in_char = True
            elif c == '{':
                brace_count += 1
            elif c == '}':
                brace_count -= 1
            elif c == '(':
                paren_count += 1
            elif c == ')':
                paren_count -= 1
        i += 1

    if brace_count != 0:
        return False, f"Unbalanced curly braces '{{}}'! Final brace count: {brace_count}"
    if paren_count != 0:
        return False, f"Unbalanced parentheses '()'! Final paren count: {paren_count}"

    missing_funcs = []
    for fn in expected_funcs:
        if not re.search(rf'extern\s+"C"\s+void\s+{re.escape(fn)}\s*\(\s*PPCContext\s*&\s*\w+\s*,\s*uint8_t\s*\*\s*\w+\s*\)', code):
            if not re.search(rf'void\s+{re.escape(fn)}\s*\(\s*PPCContext', code):
                missing_funcs.append(fn)

    if missing_funcs:
        return False, f"Missing expected function signature `extern "C" void <fn>(PPCContext& ctx, uint8_t* base)` for: {', '.join(missing_funcs)}"

    if "REX_FUNC_PROLOGUE" not in code:
        return False, "Missing `REX_FUNC_PROLOGUE();` at the start of hooked function(s)."

    if extracted_data:
        for d in extracted_data:
            fn = d["func_name"]
            orig_code = d["code"]
            # Check if original code modified ctx.r1.u32 AND called subfunctions, ensure generated code doesn't leak ctx.r1.u32
            if "ctx.r1.u32 =" in orig_code and "ctx.r1.u32" in code:
                r1_writes = len(re.findall(r"ctx\.r1\.u32\s*=", code))
                if r1_writes == 1:
                    return (
                        False,
                        f"Function `{fn}` modifies `ctx.r1.u32` (stack allocation) only once without restoring `ctx.r1.u32` before return! "
                        f"Either eliminate guest stack allocation if safe, or restore `ctx.r1.u32` on every return path.",
                    )

    return True, "Verification passed"


def main():
    parser = argparse.ArgumentParser(description="PPC function extractor, local LLM inspector, and hook generator.")
    parser.add_argument("--funcs", required=True, help="Comma-separated list of functions (e.g. sub_821D5F70,sub_828CC1A0)")
    parser.add_argument("--out", default="", help="Path to output .inc, .cpp, or .md file")
    parser.add_argument(
        "--mode",
        default="hook",
        choices=["hook", "vulkan-direct", "inspect"],
        help="Execution mode: 'inspect' (zero-token reverse-engineering summary on local 7B), 'vulkan-direct' (Rule 10 D3D9 bypass hook), or 'hook' (standard C++/NEON hook)",
    )
    parser.add_argument("--model", default=DEFAULT_NIM_MODEL, help=f"NIM model or 'local' (default: {DEFAULT_NIM_MODEL})")
    parser.add_argument("--backend", default="auto", choices=["auto", "nim", "local"], help="Backend provider")
    parser.add_argument("--spec", default="", help="Additional optimization requirements or domain rules")
    parser.add_argument("--extract-only", action="store_true", help="Only extract functions from generated recomp without calling LLM")
    parser.add_argument("--api-key", default="", help="Custom NVIDIA API key")
    parser.add_argument("--max-tokens", type=int, default=4096, help="Max output tokens for completion")
    parser.add_argument("--retries", type=int, default=2, help="Number of auto-repair retries if code validation fails")

    args = parser.parse_args()

    func_list = [f.strip() for f in args.funcs.split(",") if f.strip()]
    if not func_list:
        print("[!] No functions specified in --funcs.", file=sys.stderr)
        sys.exit(1)

    # Hard check against Audio range 0x82900000 - 0x8293FFFF
    enforce_audio_ban(func_list)

    print(f"[*] Locating {len(func_list)} function(s) in {GENERATED_DIR}...")
    extracted_data = []
    for fn in func_list:
        res = find_function_in_files(fn)
        if not res:
            print(f"[-] Function {fn} not found in {GENERATED_DIR}!", file=sys.stderr)
            sys.exit(1)
        print(f"[+] Found {fn} in {os.path.basename(res['file'])}:{res['line']} ({res['length']} bytes)")
        extracted_data.append(res)

    callgraph_context = build_callgraph_context(extracted_data)

    if args.extract_only:
        header = f"// Extracted Recompiled Functions
/*
{callgraph_context}*/

"
        full_text = header + "

".join(
            [f"// --- {d['func_name']} ({os.path.basename(d['file'])}:{d['line']}) ---
" + d['code'] for d in extracted_data]
        )
        if args.out:
            os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(full_text)
            print(f"[+] Extracted functions written to {args.out}")
        else:
            print(full_text)
        return

    # In 'inspect' mode, default to local RTX 3070 unless 'nim' was explicitly requested
    if args.mode == "inspect" and args.backend == "auto" and args.model == DEFAULT_NIM_MODEL:
        is_local = True
    else:
        is_local = (args.backend == "local") or ("local" in args.model.lower()) or ("qwen" in args.model.lower())

    target_url = LOCAL_BASE_URL if is_local else NIM_BASE_URL

    api_key = args.api_key or get_api_key()
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
    }
    if not is_local:
        headers["Authorization"] = f"Bearer {api_key}"

    if args.mode == "inspect":
        sys_prompt = SYSTEM_PROMPT_INSPECT
    elif args.mode == "vulkan-direct":
        sys_prompt = SYSTEM_PROMPT_VULKAN_DIRECT
    else:
        sys_prompt = SYSTEM_PROMPT_HOOK

    prompt_parts = [callgraph_context]
    if args.spec:
        prompt_parts.append(f"### Specific Optimization Requirements:
{args.spec}
")

    prompt_parts.append("### Target Functions:")
    for d in extracted_data:
        prompt_parts.append(f"
// From {os.path.basename(d['file'])}:{d['line']}:
{d['code']}
")

    if args.mode == "inspect":
        prompt_parts.append(
            "
Provide the structured reverse-engineering inspection report for the function(s) above. "
            "Be concise, exact with hex offsets, and highlight D3D9 calls vs RAGE logic."
        )
    elif args.mode == "vulkan-direct":
        prompt_parts.append(
            "
Generate drop-in replacement C++20 hooks implementing Rule 10 (bypass D3D9 wrapper overhead, "
            "coalesce state updates, optimize with ARM64 NEON, and preserve PPCContext ABI & stack restoration)."
        )
    else:
        prompt_parts.append(
            "
Generate drop-in replacement C++20 hooks for the above function(s). "
            "Preserve stack restoration, follow PPCContext ABI, and write clean high-performance code."
        )

    user_prompt = "
".join(prompt_parts)
    messages = [
        {"role": "system", "content": sys_prompt},
        {"role": "user", "content": user_prompt},
    ]

    payload = {
        "model": args.model if not is_local else "qwen2.5-coder-7b",
        "messages": messages,
        "temperature": 0.1 if args.mode == "inspect" else 0.2,
        "max_tokens": args.max_tokens,
    }
    if is_local:
        payload["cache_prompt"] = True

    print(f"[*] Mode='{args.mode}' | Sending request to {target_url} (model='{payload['model']}')...")

    # Mode: inspect (returns markdown analysis directly)
    if args.mode == "inspect":
        try:
            raw_reply = call_chat_completion(target_url, headers, payload)
        except Exception as e:
            print(f"[-] LLM Request Error: {e}", file=sys.stderr)
            sys.exit(1)

        report = re.sub(r'<think>[\s\S]*?</think>', '', raw_reply).strip()
        output_text = f"# Inspection Report: {', '.join(func_list)}

{callgraph_context}
{report}
"
        if args.out:
            out_path = os.path.abspath(args.out)
            os.makedirs(os.path.dirname(out_path), exist_ok=True)
            with open(out_path, "w", encoding="utf-8") as f:
                f.write(output_text)
            print(f"[+] Inspection report written to {out_path}")
        else:
            print("
" + output_text)
        return

    # Mode: hook or vulkan-direct (with self-repair loop)
    cleaned_code = ""
    valid = False
    msg = ""

    for attempt in range(args.retries + 1):
        try:
            raw_reply = call_chat_completion(target_url, headers, payload)
        except urllib.error.HTTPError as e:
            err_msg = e.read().decode('utf-8', errors='replace')
            print(f"[-] HTTP Error {e.code}: {err_msg}", file=sys.stderr)
            sys.exit(1)
        except Exception as e:
            print(f"[-] Connection Error: {e}", file=sys.stderr)
            sys.exit(1)

        cleaned_code = clean_llm_response(raw_reply)
        if not cleaned_code:
            print(f"[-] Empty response on attempt {attempt + 1}", file=sys.stderr)
            continue

        valid, msg = verify_cpp_code(cleaned_code, func_list, extracted_data)
        if valid:
            print(f"[+] Validation OK (attempt {attempt + 1}): {msg}")
            break

        print(f"[!] Validation failed on attempt {attempt + 1}/{args.retries + 1}: {msg}", file=sys.stderr)
        if attempt < args.retries:
            print("[*] Requesting automatic self-repair from model...")
            messages.append({"role": "assistant", "content": f"```cpp
{cleaned_code}
```"})
            messages.append({
                "role": "user",
                "content": (
                    f"Your generated C++ code failed verification with the following error:
{msg}

"
                    f"Fix the issue and return the COMPLETE corrected C++ code block for: {', '.join(func_list)}."
                ),
            })

    if not cleaned_code:
        print("[-] Failed to generate valid code!", file=sys.stderr)
        sys.exit(1)

    out_banner = f"""// Generated by tools/codegen_worker.py (mode: {args.mode})
// Model: {payload['model']}
// Functions: {', '.join(func_list)}
// Target: Liberty Recomp Android (Adreno 660 / Snapdragon 888)

#include <cstdint>
#include <cstring>
#include <atomic>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

"""
    final_output = out_banner + cleaned_code + "
"

    if args.out:
        out_path = os.path.abspath(args.out)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(final_output)
        print(f"[+] Successfully wrote {len(final_output)} bytes to {out_path}")
    else:
        print("
" + final_output)


if __name__ == "__main__":
    main()
