// Package 39: job-queue probe for Main XThread (step C3 of doc 38, "work-helping").
//
// Question it answers: when Main XThread blocks (11.2 ms/frame waiting for jobs, 6.5 ms for
// physics in the 30.10 trace on the 888), is there still work queued that Main could have run
// itself? Only if the answer is "often, and for many milliseconds" is work-helping worth the
// risk of running RAGE jobs on a thread they were not written for.
//
// What it records (all guest-memory reads, no guest state is changed):
//   - RAGE job workers: the worker loop sub_8285D368 receives the worker object in r3.
//     Queue Q = BE32(worker + 188) + 8; queued count = BE32(Q + 1064) (ring of 256 slots,
//     semaphore at Q + 1068, critical section at Q + 1024; see rage_job_functions_extracted).
//   - Physics queue: sub_82798038 pops from P (r3). P + 4096 != 0 means the list is non-empty.
//     The list is not walked: a stale node pointer could point at an uncommitted guest page.
//   - Physics barrier: sub_82797DB8 releases the semaphore at +36 N times (N = BE32(+0)) and
//     then waits N times on +40. SESSION_NOTES puts its call in the worker that finishes the
//     last physics item (sub_82798470 / sub_82797F20), so it is counted on any thread.
//   - Every guest wait of Main XThread (kernel entries in xboxkrnl_threading.cpp call the
//     observer below): time spent, and whether jobs / physics items were queued when it began.
//
// Output, every 5 s, Android log tag "LibertyJobProbe":
//   job-probe 5.0s: main-wait 280.0 ms/s (1210) | jobs-queued 190.0 ms/s (640, avg 3.2) |
//   phys-queued 40.0 ms/s (120) | phys-barrier 160.0 ms/s (125) | queues 1 workers 3 phys 1
// Divide ms/s by FPS to get ms per frame.
//
// --job_probe=false turns it off (the hooks then only forward to the original functions).
//
// Package 40 adds work-helping (C3 step 2), off by default: --job_help=true. Before Main
// XThread blocks on a wait without a timeout (kernel side: WaitHelpingJobs in
// xboxkrnl_threading.cpp polls the objects first), it takes one job from the RAGE queue and
// runs it exactly as the worker loop sub_8285D368 does:
//   - one semaphore count Q+1068 with a zero timeout (NtWaitForSingleObjectEx, like the
//     worker's sub_828497D8 but without blocking), so the counts and the ring stay paired;
//   - critical section Q+1024 (sub_8285FE78 / sub_8285FEE0), head Q+1060 += 1 (wraps at 256),
//     count Q+1064 -= 1, job = slot[head]. A null slot is a worker's exit sentinel: it is left
//     in place, the count is given back (sub_82849860) and helping stops for good;
//   - a worker block of its own (192 bytes, sub_8285D488 layout, allocated once with the
//     game's allocator sub_821B3510 like sub_8285D500 does): +20 = job+156 (done event),
//     memcpy(+32, job, 156), +96 = +28 (per-worker scratch of BE32(worker+24) bytes, also
//     allocated once), call BE32(+36) with r3 = +40 and lr = 0x8285D408;
//   - NtSetEvent(+20) through sub_82A13160.
// The PPCContext is copied before and restored after (FPCR too), the job runs on Main's guest
// stack 512 bytes below the waiting frame. Nested waits inside a helped job block normally.
// The 5-s line then also shows "helped N (X ms/s)": that time is inside main-wait.
// Risk: a job that waits for something Main does only after its own wait returns would
// deadlock here. RAGE jobs are written to run on any worker, but that is not proven: hence off
// by default, and the probe numbers decide whether to turn it on.

#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "gta4_init.h"

REXCVAR_DEFINE_BOOL(job_probe, true, "Diagnostics",
                    "Log every 5 s how long Main XThread waits while RAGE jobs or physics "
                    "items are still queued (package 39, work-helping feasibility)");
REXCVAR_DEFINE_BOOL(job_help, true, "GTA IV/Performance",
                    "Experimental work-helping: before Main XThread blocks on a wait without a "
                    "timeout it runs queued RAGE jobs itself (package 40)");
REXCVAR_DEFINE_BOOL(gta4_atomic_sections_lock, false, "GTA IV/Performance",
                    "51: the guest lwarx/stwcx. sections in game code (atomic_sections.py) take "
                    "the host global lock again, as before 51 (A/B in the same build)");

// 53: wait spin budgets of the RAGE worker threads (librexruntime threading_posix.cpp,
// rex_thread_set_wait_spin). 888, batch 52 (M1_control): Main XThread wakes the job workers
// ~54 times a frame; each wake is a futex syscall on Main (1.27 ms of its frame) and the worker
// runs 233 us later on average. 53 % of those sleeps are shorter than 200 us (59 % < 300 us);
// with a budget a spinning worker takes the count at once and Main's release needs no syscall.
// 200 us costs ~6.6 ms of little-core time a frame for the three workers (300 us: ~9 ms; the
// little cores are ~70 % busy while playing, so the default stays at 200). Physics
// workers wake each other ~51 times a frame; 96 % of those sleeps last less than 100 us.
REXCVAR_DEFINE_UINT32(gta4_job_worker_spin_us, 200, "GTA IV/Performance",
                      "53: RAGE job workers spin up to this long (us) in a wait before they "
                      "sleep (0 = only the global 30 us spin, as before 53; max 2000)");
REXCVAR_DEFINE_UINT32(gta4_physics_worker_spin_us, 100, "GTA IV/Performance",
                      "53: as gta4_job_worker_spin_us, for the RAGE physics workers");
REXCVAR_DEFINE_BOOL(gta4_worker_spin_wfe, true, "GTA IV/Performance",
                    "53: worker spins wait in WFE (core clock-gated until the object changes) "
                    "instead of a busy loop; used only when the kernel's event stream is on");
REXCVAR_DEFINE_BOOL(gta4_job_worker_spin_little_only, true, "GTA IV/Performance",
                    "53: job workers use their spin budget only while on a little core (on a big "
                    "core a spinner would hold a core Main XThread / RenderGuest / audio want)");

// 51: read by the generated code (tools/atomic_sections.py) with __atomic_load_n, once per
// section. Set from the cvar when the first RAGE job worker starts (early in boot); until
// then the sections run without the lock, which is correct either way.
extern "C" __attribute__((visibility("default"))) bool gta4_atomic_sections_lock_flag = false;

extern "C" {
typedef uint64_t (*RexGuestWaitObserverFn)(int phase, uint64_t token);
extern RexGuestWaitObserverFn rex_guest_wait_observer;
typedef int (*RexGuestWaitHelperFn)(void* ppc_context);
extern RexGuestWaitHelperFn rex_guest_wait_helper;
void rex_thread_set_wait_spin(uint32_t spin_us, uint32_t flags);  // 53, threading_posix.cpp
}

namespace gta4::jobprobe {
namespace {

constexpr int kMaxQueues = 8;
constexpr uint32_t kQueueCountOffset = 1064;
constexpr uint32_t kWorkerQueueOffset = 188;
constexpr uint32_t kPhysicsListOffset = 4096;
constexpr uint64_t kLogPeriodNs = 5'000'000'000ull;

std::atomic<uint8_t*> g_base{nullptr};
std::atomic<uint32_t> g_job_queues[kMaxQueues];
std::atomic<uint32_t> g_job_queue_count{0};
std::atomic<uint32_t> g_job_workers{0};
std::atomic<uint32_t> g_phys_queues[kMaxQueues];
std::atomic<uint32_t> g_phys_queue_count{0};
std::atomic<uint32_t> g_first_worker{0};

// Worker and queue layout (sub_8285D488 / sub_8285D500 / sub_8285D610 / sub_8285D368).
constexpr uint32_t kQueueCsOffset = 1024;
constexpr uint32_t kQueueHeadOffset = 1060;
constexpr uint32_t kQueueSemaphoreOffset = 1068;
constexpr uint32_t kQueueSlots = 256;
constexpr uint32_t kWorkerSize = 192;
constexpr uint32_t kWorkerEventOffset = 20;
constexpr uint32_t kWorkerScratchSizeOffset = 24;
constexpr uint32_t kWorkerScratchOffset = 28;
constexpr uint32_t kWorkerDescriptorOffset = 32;
constexpr uint32_t kDescriptorSize = 156;
constexpr uint32_t kDescriptorFunctionOffset = 4;
constexpr uint32_t kDescriptorArgumentOffset = 8;
constexpr uint32_t kDescriptorScratchOffset = 64;
constexpr uint32_t kJobEventOffset = 156;
constexpr uint32_t kWorkerCallReturnAddress = 0x8285D408;
constexpr uint32_t kMaxScratchBytes = 1u << 20;

// Main XThread only.
thread_local bool t_in_help = false;
std::atomic<bool> g_help_off{false};
uint32_t g_help_block = 0;

// Written only by Main XThread (the kernel observer runs only on that thread).
struct MainStats {
  uint64_t window_start_ns = 0;
  uint64_t waits = 0, wait_ns = 0;
  uint64_t job_waits = 0, job_wait_ns = 0, job_queued_sum = 0;
  uint64_t phys_waits = 0, phys_wait_ns = 0;
  uint64_t helped = 0, helped_ns = 0;
  uint32_t pending_queued = 0;  // queued count at the begin of the current wait
};
MainStats g_main;
std::atomic<uint64_t> g_barriers{0}, g_barrier_ns{0};

uint64_t NowNs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

void AddUnique(std::atomic<uint32_t>* table, std::atomic<uint32_t>& count, uint32_t value) {
  if (!value) return;
  const uint32_t n = count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && i < kMaxQueues; ++i) {
    if (table[i].load(std::memory_order_relaxed) == value) return;
  }
  // Rare (once per worker thread): a CAS loop is enough.
  uint32_t slot = count.load(std::memory_order_relaxed);
  while (slot < kMaxQueues) {
    if (count.compare_exchange_weak(slot, slot + 1, std::memory_order_acq_rel)) {
      table[slot].store(value, std::memory_order_release);
      return;
    }
  }
}

uint32_t QueuedJobs(uint8_t* base) {
  uint32_t total = 0;
  const uint32_t n = g_job_queue_count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && i < kMaxQueues; ++i) {
    const uint32_t q = g_job_queues[i].load(std::memory_order_acquire);
    if (!q) continue;
    const uint32_t c = REX_LOAD_U32(q + kQueueCountOffset);
    if (c <= 256) total += c;  // a torn or garbage value is ignored
  }
  return total;
}

bool PhysicsQueued(uint8_t* base) {
  const uint32_t n = g_phys_queue_count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && i < kMaxQueues; ++i) {
    const uint32_t p = g_phys_queues[i].load(std::memory_order_acquire);
    if (p && REX_LOAD_U32(p + kPhysicsListOffset) != 0) return true;
  }
  return false;
}

double MsPerSecond(uint64_t ns, uint64_t window_ns) {
  return window_ns ? (static_cast<double>(ns) / 1e6) * (1e9 / static_cast<double>(window_ns))
                   : 0.0;
}

void MaybeLog(uint64_t now) {
  MainStats& s = g_main;
  if (!s.window_start_ns) {
    s.window_start_ns = now;
    return;
  }
  const uint64_t window = now - s.window_start_ns;
  if (window < kLogPeriodNs) return;
  __android_log_print(
      ANDROID_LOG_WARN, "LibertyJobProbe",
      "job-probe %.1fs: main-wait %.1f ms/s (%llu) | jobs-queued %.1f ms/s (%llu, avg %.1f) | "
      "phys-queued %.1f ms/s (%llu) | phys-barrier %.1f ms/s (%llu) | helped %llu (%.1f ms/s) | "
      "queues %u workers %u phys %u",
      static_cast<double>(window) / 1e9, MsPerSecond(s.wait_ns, window),
      static_cast<unsigned long long>(s.waits), MsPerSecond(s.job_wait_ns, window),
      static_cast<unsigned long long>(s.job_waits),
      s.job_waits ? static_cast<double>(s.job_queued_sum) / static_cast<double>(s.job_waits)
                  : 0.0,
      MsPerSecond(s.phys_wait_ns, window), static_cast<unsigned long long>(s.phys_waits),
      MsPerSecond(g_barrier_ns.exchange(0, std::memory_order_relaxed), window),
      static_cast<unsigned long long>(g_barriers.exchange(0, std::memory_order_relaxed)),
      static_cast<unsigned long long>(s.helped), MsPerSecond(s.helped_ns, window),
      g_job_queue_count.load(), g_job_workers.load(), g_phys_queue_count.load());
  s = MainStats{};
  s.window_start_ns = now;
}

// Token: bit 0 jobs queued, bit 1 physics queued, bit 2 always set, bits 3.. begin time.
uint64_t Observer(int phase, uint64_t token) {
  uint8_t* base = g_base.load(std::memory_order_acquire);
  if (!base) return 0;
  if (phase == 0) {
    if (t_in_help || !REXCVAR_GET(job_probe)) return 0;  // waits inside a helped job
    const uint32_t queued = QueuedJobs(base);
    const bool phys = PhysicsQueued(base);
    g_main.pending_queued = queued;
    return (NowNs() << 3) | 4u | (phys ? 2u : 0u) | (queued ? 1u : 0u);
  }
  const uint64_t now = NowNs();
  const uint64_t begin = token >> 3;
  const uint64_t dur = now > begin ? now - begin : 0;
  MainStats& s = g_main;
  ++s.waits;
  s.wait_ns += dur;
  if (token & 1u) {
    ++s.job_waits;
    s.job_wait_ns += dur;
    s.job_queued_sum += g_main.pending_queued;
  }
  if (token & 2u) {
    ++s.phys_waits;
    s.phys_wait_ns += dur;
  }
  MaybeLog(now);
  return 0;
}

void DisableHelp(const char* reason) {
  if (!g_help_off.exchange(true, std::memory_order_relaxed)) {
    __android_log_print(ANDROID_LOG_WARN, "LibertyJobProbe", "job-help: off (%s)", reason);
  }
}

// One job from queue q, exactly as the worker loop runs it. ctx.r1 is already below the
// waiting frame; the caller restores the whole context afterwards.
bool RunOneJob(PPCContext& ctx, uint8_t* base, uint32_t q, uint32_t worker) {
  if (!g_help_block) {
    const uint32_t scratch_size = REX_LOAD_U32(worker + kWorkerScratchSizeOffset);
    if (scratch_size > kMaxScratchBytes) {
      DisableHelp("worker scratch too large");
      return false;
    }
    ctx.r3.u64 = kWorkerSize + 16;
    sub_821B3510(ctx, base);
    const uint32_t block = ctx.r3.u32;
    uint32_t scratch = 0;
    if (block && scratch_size) {
      ctx.r3.u64 = scratch_size;
      sub_821B3510(ctx, base);
      scratch = ctx.r3.u32;
    }
    if (!block || (scratch_size && !scratch)) {
      DisableHelp("allocation failed");
      return false;
    }
    std::memset(REX_RAW_ADDR(block), 0, kWorkerSize + 16);
    REX_STORE_U32(block + kWorkerScratchSizeOffset, scratch_size);
    REX_STORE_U32(block + kWorkerScratchOffset, scratch);
    REX_STORE_U32(block + 188, REX_LOAD_U32(worker + 188));
    g_help_block = block;
    __android_log_print(ANDROID_LOG_WARN, "LibertyJobProbe",
                        "job-help: ready (worker block %08X, scratch %u bytes at %08X)", block,
                        scratch_size, scratch);
  }
  const uint32_t block = g_help_block;
  const uint32_t timeout_address = block + kWorkerSize;  // 8 bytes of zero: poll
  REX_STORE_U64(timeout_address, 0);
  const uint32_t semaphore = REX_LOAD_U32(q + kQueueSemaphoreOffset);

  // 1. One semaphore count, without blocking (r4 = wait mode 1, r5 = not alertable).
  ctx.r3.u64 = semaphore;
  ctx.r4.u64 = 1;
  ctx.r5.u64 = 0;
  ctx.r6.u64 = timeout_address;
  __imp__NtWaitForSingleObjectEx(ctx, base);
  if (ctx.r3.u32 != 0) {
    return false;  // a worker was faster (STATUS_TIMEOUT)
  }

  // 2. Pop under the queue's critical section.
  ctx.r3.u64 = q + kQueueCsOffset;
  sub_8285FE78(ctx, base);
  uint32_t head = REX_LOAD_U32(q + kQueueHeadOffset) + 1;
  if (head == kQueueSlots) head = 0;
  const uint32_t job = REX_LOAD_U32(q + head * 4);
  if (!job) {
    // A worker's exit sentinel: leave it and the count for the worker.
    ctx.r3.u64 = q + kQueueCsOffset;
    sub_8285FEE0(ctx, base);
    ctx.r3.u64 = semaphore;
    sub_82849860(ctx, base);
    DisableHelp("queue shutting down");
    return false;
  }
  REX_STORE_U32(q + kQueueHeadOffset, head);
  REX_STORE_U32(q + kQueueCountOffset, REX_LOAD_U32(q + kQueueCountOffset) - 1);
  ctx.r3.u64 = q + kQueueCsOffset;
  sub_8285FEE0(ctx, base);

  // 3. The worker's call sequence.
  REX_STORE_U32(block + kWorkerEventOffset, REX_LOAD_U32(job + kJobEventOffset));
  std::memcpy(REX_RAW_ADDR(block + kWorkerDescriptorOffset), REX_RAW_ADDR(job), kDescriptorSize);
  REX_STORE_U32(block + kWorkerDescriptorOffset + kDescriptorScratchOffset,
                REX_LOAD_U32(block + kWorkerScratchOffset));
  const uint32_t function =
      REX_LOAD_U32(block + kWorkerDescriptorOffset + kDescriptorFunctionOffset);
  ctx.r3.u64 = block + kWorkerDescriptorOffset + kDescriptorArgumentOffset;
  ctx.r4.u64 = job;
  ctx.lr = kWorkerCallReturnAddress;
  REX_CALL_INDIRECT_FUNC(function);
  ctx.r3.u64 = REX_LOAD_U32(block + kWorkerEventOffset);
  sub_82A13160(ctx, base);
  return true;
}

int Helper(void* ppc_context) {
  if (t_in_help || g_help_off.load(std::memory_order_relaxed) || !REXCVAR_GET(job_help)) {
    return 0;
  }
  uint8_t* base = g_base.load(std::memory_order_acquire);
  const uint32_t worker = g_first_worker.load(std::memory_order_acquire);
  if (!base || !worker || !ppc_context) return 0;
  // A queue with work (unlocked look; the semaphore decides).
  uint32_t q = 0;
  const uint32_t n = g_job_queue_count.load(std::memory_order_acquire);
  for (uint32_t i = 0; i < n && i < kMaxQueues; ++i) {
    const uint32_t candidate = g_job_queues[i].load(std::memory_order_acquire);
    if (!candidate) continue;
    const uint32_t c = REX_LOAD_U32(candidate + kQueueCountOffset);
    if (c && c <= kQueueSlots) {
      q = candidate;
      break;
    }
  }
  if (!q) return 0;

  PPCContext& ctx = *static_cast<PPCContext*>(ppc_context);
  t_in_help = true;
  const PPCContext saved = ctx;
  const uint64_t begin = NowNs();
  ctx.r1.u64 = (saved.r1.u32 - 512u) & ~0xFu;
  const bool ran = RunOneJob(ctx, base, q, worker);
  ctx = saved;
  if (ctx.fpscr.getcsr() != saved.fpscr.csr) {
    ctx.fpscr.setcsr(saved.fpscr.csr);  // the job may have changed the host rounding mode
  }
  t_in_help = false;
  if (ran) {
    ++g_main.helped;
    g_main.helped_ns += NowNs() - begin;
  }
  return ran ? 1 : 0;
}

void Install(uint8_t* base) {
  const bool probe = REXCVAR_GET(job_probe);
  const bool help = REXCVAR_GET(job_help);
  if (!probe && !help) return;
  uint8_t* expected = nullptr;
  if (g_base.compare_exchange_strong(expected, base, std::memory_order_acq_rel)) {
    if (probe) __atomic_store_n(&rex_guest_wait_observer, &Observer, __ATOMIC_RELEASE);
    if (help) __atomic_store_n(&rex_guest_wait_helper, &Helper, __ATOMIC_RELEASE);
    __android_log_print(ANDROID_LOG_WARN, "LibertyJobProbe", "job-probe: installed (probe %d, help %d)",
                        probe ? 1 : 0, help ? 1 : 0);
  }
}

// 53: the calling worker thread's wait spin (cvar 0 = leave the global budget).
// Flags of rex_thread_set_wait_spin: bit 0 WFE, bit 1 only on little cores.
void SetWorkerSpin(uint32_t spin_us, bool little_only) {
  if (!spin_us) return;
  rex_thread_set_wait_spin(spin_us,
                           (REXCVAR_GET(gta4_worker_spin_wfe) ? 1u : 0u) | (little_only ? 2u : 0u));
}

}  // namespace
}  // namespace gta4::jobprobe

// RAGE job worker loop. Runs for the life of the worker thread; only the entry is observed.
REX_HOOK_RAW(sub_8285D368) {
  using namespace gta4::jobprobe;
  __atomic_store_n(&gta4_atomic_sections_lock_flag, REXCVAR_GET(gta4_atomic_sections_lock),
                   __ATOMIC_RELAXED);  // 51
  if (REXCVAR_GET(job_probe) || REXCVAR_GET(job_help)) {
    const uint32_t worker = ctx.r3.u32;
    if (worker) {
      g_job_workers.fetch_add(1, std::memory_order_relaxed);
      AddUnique(g_job_queues, g_job_queue_count, REX_LOAD_U32(worker + kWorkerQueueOffset) + 8);
      uint32_t expected = 0;
      g_first_worker.compare_exchange_strong(expected, worker, std::memory_order_acq_rel);
    }
    Install(base);
  }
  SetWorkerSpin(REXCVAR_GET(gta4_job_worker_spin_us),  // 53
                REXCVAR_GET(gta4_job_worker_spin_little_only));
  __imp__sub_8285D368(ctx, base);
}

// 53: RAGE physics worker thread (entry of every RagePhysics thread). Only the wait spin of the
// thread is set; the loop itself is the original. Physics spins on any core: its budget is
// short, and Main XThread waits for the physics step (2.6 ms a frame), half of which runs on
// the big cores.
REX_HOOK_RAW(sub_827989F8) {
  gta4::jobprobe::SetWorkerSpin(REXCVAR_GET(gta4_physics_worker_spin_us), false);
  __imp__sub_827989F8(ctx, base);
}

// Physics item pop (physics workers). Records the queue object once.
REX_HOOK_RAW(sub_82798038) {
  using namespace gta4::jobprobe;
  if (REXCVAR_GET(job_probe)) {
    AddUnique(g_phys_queues, g_phys_queue_count, ctx.r3.u32);
    Install(base);
  }
  __imp__sub_82798038(ctx, base);
}

// Physics barrier: release N waiters, wait for N acknowledgements (any thread).
REX_HOOK_RAW(sub_82797DB8) {
  using namespace gta4::jobprobe;
  if (!REXCVAR_GET(job_probe) || !g_base.load(std::memory_order_relaxed)) {
    __imp__sub_82797DB8(ctx, base);
    return;
  }
  const uint64_t begin = NowNs();
  __imp__sub_82797DB8(ctx, base);
  g_barriers.fetch_add(1, std::memory_order_relaxed);
  g_barrier_ns.fetch_add(NowNs() - begin, std::memory_order_relaxed);
}
