#!/usr/bin/env python3
"""
tools/codegen_worker.py - Automated PPC function extractor, LLM decompiler, and hook generator.

Supports both NVIDIA NIM API (cloud) and llama-server (local RTX 3070 Qwen2.5-Coder-7B).

Usage:
  python tools/codegen_worker.py --funcs sub_821D5F70 --out glue/rexglue-sdk-main/gta4-recomp/src/gta4_test.inc
  python tools/codegen_worker.py --funcs sub_828CC1A0,sub_828C4338 --out hooks.inc --model poolside/laguna-xs-2.1
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
    "nvidia/nemotron-3.5-bolt-e4b",
]

DEFAULT_NIM_KEY = "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"
NIM_BASE_URL = "https://integrate.api.nvidia.com/v1/chat/completions"
LOCAL_BASE_URL = "http://127.0.0.1:8080/v1/chat/completions"

WORKSPACE_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
GENERATED_DIR = os.path.join(WORKSPACE_ROOT, "glue", "rexglue-sdk-main", "gta4-recomp", "generated")

SYSTEM_PROMPT = """You are an expert low-level reverse engineer, compiler engineer, and ARM64 systems optimizer for Xbox 360 (PowerPC) recompilation projects running on Android / Snapdragon 888.

Your task is to take decompiled / recompiled PowerPC guest functions and produce high-performance, drop-in replacement C++ hooks.

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
   - `ctx.r1.u32`: guest stack pointer. If the guest function modifies `ctx.r1` (e.g. allocating stack `ctx.r1.u32 = ea`), the original stack pointer MUST be restored before return, matching the guest function epilogue.
   - `ctx.lr`: link register (return address).
4. **Memory Addressing & Big-Endian Conversion**:
   - Xbox 360 is Big-Endian. All loads and stores from guest memory must use recomp helpers:
     - `REX_LOAD_U32(ea)`, `REX_STORE_U32(ea, val)`
     - `REX_LOAD_U64(ea)`, `REX_STORE_U64(ea, val)`
     - `REX_LOAD_F32(ea)`, `REX_STORE_F32(ea, val)`
     - `REX_RAW_ADDR(ea)` returns host `uint8_t*` pointer for guest physical address `ea`.
5. **ARM64 Optimizations**:
   - Fast paths: Avoid redundant guest stack spills / reloads for temporary variables.
   - Use ARM64 NEON (`#include <arm_neon.h>`) when vector math or matrix transforms are present.
   - Concurrency / Spinlocks: If `gta4_atomic_sections_lock_flag` is false, use relaxed/acquire-release `__atomic_*` or `std::atomic` instead of heavy global guest locks.
6. **Output Format**:
   Return ONLY clean C++ code enclosed in ```cpp and ```. Do NOT include markdown explanations outside the code block.
"""


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
        # Find opening brace '{'
        brace_start = content.find('{', match.end())
        if brace_start == -1:
            continue

        # Brace counting from brace_start
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
            # Extract line number
            line_no = content[:start_pos].count('\n') + 1
            func_code = content[start_pos:end_pos].strip()
            return {
                "func_name": func_name,
                "file": filepath,
                "line": line_no,
                "code": func_code,
                "length": len(func_code),
            }

    return None


def get_api_key() -> str:
    # 1. Environment variable
    if os.environ.get("NVIDIA_API_KEY"):
        return os.environ["NVIDIA_API_KEY"]
    
    # 2. Check mcp_config.json
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

    # 3. Default constant
    return DEFAULT_NIM_KEY


def call_chat_completion(url: str, headers: dict, payload: dict, timeout: int = 120) -> str:
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
    # Remove <think>...</think> if present
    cleaned = re.sub(r'<think>[\s\S]*?</think>', '', raw_text)
    
    # Extract ```cpp ... ``` or ```c ... ```
    code_block_match = re.search(r'```(?:cpp|c|\+\+)?\s*([\s\S]*?)```', cleaned)
    if code_block_match:
        return code_block_match.group(1).strip()
    return cleaned.strip()


def verify_cpp_code(code: str, expected_funcs: list) -> tuple:
    """
    Verifies that the generated C++ code:
    1. Has balanced braces.
    2. Contains extern "C" void for each expected function.
    """
    brace_count = 0
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
        i += 1

    if brace_count != 0:
        return False, f"Unbalanced braces! Final brace count: {brace_count}"

    missing_funcs = []
    for fn in expected_funcs:
        if not re.search(rf'extern\s+"C"\s+void\s+{re.escape(fn)}\s*\(', code):
            # Check without extern "C"
            if not re.search(rf'void\s+{re.escape(fn)}\s*\(\s*PPCContext', code):
                missing_funcs.append(fn)

    if missing_funcs:
        return False, f"Missing expected function signatures: {', '.join(missing_funcs)}"

    return True, "Verification passed"


def main():
    parser = argparse.ArgumentParser(description="PPC function extractor and LLM decompiler/hook generator.")
    parser.add_argument("--funcs", required=True, help="Comma-separated list of functions (e.g. sub_821D5F70,sub_828CC1A0)")
    parser.add_argument("--out", default="", help="Path to output .inc or .cpp file")
    parser.add_argument("--model", default=DEFAULT_NIM_MODEL, help=f"NIM model or 'local' (default: {DEFAULT_NIM_MODEL})")
    parser.add_argument("--backend", default="auto", choices=["auto", "nim", "local"], help="Backend provider")
    parser.add_argument("--spec", default="", help="Additional optimization requirements or domain rules")
    parser.add_argument("--extract-only", action="store_true", help="Only extract functions from generated recomp without calling LLM")
    parser.add_argument("--api-key", default="", help="Custom NVIDIA API key")
    parser.add_argument("--max-tokens", type=int, default=4096, help="Max output tokens for completion")

    args = parser.parse_args()

    func_list = [f.strip() for f in args.funcs.split(",") if f.strip()]
    if not func_list:
        print("[!] No functions specified in --funcs.", file=sys.stderr)
        sys.exit(1)

    print(f"[*] Locating {len(func_list)} function(s) in {GENERATED_DIR}...")
    extracted_data = []
    for fn in func_list:
        res = find_function_in_files(fn)
        if not res:
            print(f"[-] Function {fn} not found in {GENERATED_DIR}!", file=sys.stderr)
            sys.exit(1)
        print(f"[+] Found {fn} in {os.path.basename(res['file'])}:{res['line']} ({res['length']} bytes)")
        extracted_data.append(res)

    if args.extract_only:
        header = "// Extracted Recompiled Functions\n\n"
        full_text = header + "\n\n".join([f"// --- {d['func_name']} ({os.path.basename(d['file'])}:{d['line']}) ---\n" + d['code'] for d in extracted_data])
        if args.out:
            os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(full_text)
            print(f"[+] Extracted functions written to {args.out}")
        else:
            print(full_text)
        return

    # Determine backend
    is_local = (args.backend == "local") or ("local" in args.model.lower()) or ("qwen" in args.model.lower())
    target_url = LOCAL_BASE_URL if is_local else NIM_BASE_URL

    api_key = args.api_key or get_api_key()
    headers = {
        "Content-Type": "application/json",
        "Accept": "application/json",
    }
    if not is_local:
        headers["Authorization"] = f"Bearer {api_key}"

    # Build prompt
    prompt_parts = []
    if args.spec:
        prompt_parts.append(f"### Specific Optimization Requirements:\n{args.spec}\n")

    prompt_parts.append("### Target Functions to Optimize and Hook:")
    for d in extracted_data:
        prompt_parts.append(f"\n// From {os.path.basename(d['file'])}:{d['line']}:\n{d['code']}\n")

    prompt_parts.append("\nGenerate drop-in replacement C++ hooks for the above function(s). Remember to preserve stack restoration, follow PPCContext ABI, and write clean high-performance code.")
    user_prompt = "\n".join(prompt_parts)

    payload = {
        "model": args.model if not is_local else "qwen2.5-coder-7b",
        "messages": [
            {"role": "system", "content": SYSTEM_PROMPT},
            {"role": "user", "content": user_prompt}
        ],
        "temperature": 0.2,
        "max_tokens": args.max_tokens,
    }

    print(f"[*] Sending codegen request to {target_url} using model '{payload['model']}'...")
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
        print("[-] Received empty response from model!", file=sys.stderr)
        sys.exit(1)

    print("[*] Validating generated C++ code...")
    valid, msg = verify_cpp_code(cleaned_code, func_list)
    if not valid:
        print(f"[!] Warning during validation: {msg}", file=sys.stderr)
    else:
        print(f"[+] Validation OK: {msg}")

    # Add header banner
    out_banner = f"""// Generated by tools/codegen_worker.py
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
    final_output = out_banner + cleaned_code + "\n"

    if args.out:
        out_path = os.path.abspath(args.out)
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            f.write(final_output)
        print(f"[+] Successfully wrote {len(final_output)} bytes to {out_path}")
    else:
        print("\n" + final_output)


if __name__ == "__main__":
    main()
