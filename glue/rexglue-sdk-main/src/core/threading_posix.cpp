/**
 * @file        core/threading_posix.cpp
 * @brief       POSIX platform threading implementations
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 * @license     BSD 3-Clause License
 */

#include <rex/platform.h>
#include <rex/thread.h>

static_assert(REX_PLATFORM_LINUX || REX_PLATFORM_MAC, "This file is POSIX-only");

#include <signal.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <limits>
#include <memory>

#include <pthread.h>
#include <cstring>
#if REX_PLATFORM_ANDROID
#include <android/log.h>
#endif
#include <semaphore.h>
#include <sys/eventfd.h>
#if REX_PLATFORM_ANDROID
#include <sys/prctl.h>
#endif
#include <sys/syscall.h>
#if defined(__linux__) || defined(__ANDROID__)
#include <sys/auxv.h>  // 53: getauxval (HWCAP_EVTSTRM)
#endif
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <rex/assert.h>
#include <rex/chrono/chrono_steady_cast.h>
#include <rex/logging.h>
#include <rex/thread/timer_queue.h>

#include <sched.h>

#if REX_PLATFORM_ANDROID
#include <dlfcn.h>

#include <rex/main_android.h>
#include <rex/math.h>
#include <rex/string.h>
#endif

#if REX_PLATFORM_LINUX
// SIGEV_THREAD_ID in timer_create(...) is a Linux extension
#define REX_HAS_SIGEV_THREAD_ID 1
#ifdef __GLIBC__
#define sigev_notify_thread_id _sigev_un._tid
#endif
#if __GLIBC__ == 2 && __GLIBC_MINOR__ < 30
#define gettid() syscall(SYS_gettid)
#endif
#else
#define REX_HAS_SIGEV_THREAD_ID 0
#endif

namespace rex::thread {

#if REX_PLATFORM_ANDROID
// May be null if no dynamically loaded functions are required.
static void* android_libc_;
// API 26+.
static int (*android_pthread_getname_np_)(pthread_t pthread, char* buf, size_t n);

void AndroidInitialize() {
  if (rex::GetAndroidApiLevel() >= 26) {
    android_libc_ = dlopen("libc.so", RTLD_NOW);
    assert_not_null(android_libc_);
    if (android_libc_) {
      android_pthread_getname_np_ = reinterpret_cast<decltype(android_pthread_getname_np_)>(
          dlsym(android_libc_, "pthread_getname_np"));
      assert_not_null(android_pthread_getname_np_);
    }
  }
}

void AndroidShutdown() {
  android_pthread_getname_np_ = nullptr;
  if (android_libc_) {
    dlclose(android_libc_);
    android_libc_ = nullptr;
  }
}
#endif

template <typename _Rep, typename _Period>
inline timespec DurationToTimeSpec(std::chrono::duration<_Rep, _Period> duration) {
  auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);
  auto div = ldiv(nanoseconds.count(), 1000000000L);
  return timespec{div.quot, div.rem};
}

// Thread interruption is done using user-defined signals
// This implementation uses the SIGRTMAX - SIGRTMIN to signal to a thread
// gdb tip, for SIG = SIGRTMIN + SignalType : handle SIG nostop
// lldb tip, for SIG = SIGRTMIN + SignalType : process handle SIG -s false
enum class SignalType {
  kThreadSuspend,
  kThreadUserCallback,
#if REX_PLATFORM_ANDROID
  // pthread_cancel is not available on Android, using a signal handler for
  // simplified PTHREAD_CANCEL_ASYNCHRONOUS-like behavior - not disabling
  // cancellation currently, so should be enough.
  kThreadTerminate,
#endif
  k_Count
};

int GetSystemSignal(SignalType num) {
  auto result = SIGRTMIN + static_cast<int>(num);
  assert_true(result < SIGRTMAX);
  return result;
}

SignalType GetSystemSignalType(int num) {
  return static_cast<SignalType>(num - SIGRTMIN);
}

std::array<std::atomic<bool>, static_cast<size_t>(SignalType::k_Count)> signal_handler_installed =
    {};

static void signal_handler(int signal, siginfo_t* info, void* context);

void install_signal_handler(SignalType type) {
  bool expected = false;
  if (!signal_handler_installed[static_cast<size_t>(type)].compare_exchange_strong(expected,
                                                                                   true)) {
    return;
  }
  struct sigaction action{};
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  action.sa_sigaction = signal_handler;
  sigemptyset(&action.sa_mask);
  if (sigaction(GetSystemSignal(type), &action, nullptr) != 0) {
    signal_handler_installed[static_cast<size_t>(type)] = false;
  }
}

// TODO(dougvj)
void EnableAffinityConfiguration() {}

// uint64_t ticks() { return mach_absolute_time(); }

uint32_t current_thread_system_id() {
  return static_cast<uint32_t>(syscall(SYS_gettid));
}

void MaybeYield() {
  sched_yield();
  __sync_synchronize();
}

void SyncMemory() {
  __sync_synchronize();
}

void Sleep(std::chrono::microseconds duration) {
  timespec rqtp = DurationToTimeSpec(duration);
  timespec rmtp = {};
  auto p_rqtp = &rqtp;
  auto p_rmtp = &rmtp;
  int ret = 0;
  do {
    ret = nanosleep(p_rqtp, p_rmtp);
    // Swap requested for remaining in case of signal interruption
    // in which case, we start sleeping again for the remainder
    std::swap(p_rqtp, p_rmtp);
  } while (ret == -1 && errno == EINTR);
}

// TODO(bwrsandman) Implement by allowing alert interrupts from IO operations
thread_local bool alertable_state_ = false;
bool DispatchCurrentThreadUserCallback();
SleepResult AlertableSleep(std::chrono::microseconds duration) {
  alertable_state_ = true;
  auto deadline = std::chrono::steady_clock::now() + duration;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      alertable_state_ = false;
      return SleepResult::kAlerted;
    }
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      alertable_state_ = false;
      return SleepResult::kSuccess;
    }
    auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(deadline - now);
    Sleep(std::min(remaining, std::chrono::microseconds(1000)));
  }
}

TlsHandle AllocateTlsHandle() {
  auto key = static_cast<pthread_key_t>(-1);
  auto res = pthread_key_create(&key, nullptr);
  assert_zero(res);
  assert_true(key != static_cast<pthread_key_t>(-1));
  return static_cast<TlsHandle>(key);
}

bool FreeTlsHandle(TlsHandle handle) {
  return pthread_key_delete(static_cast<pthread_key_t>(handle)) == 0;
}

uintptr_t GetTlsValue(TlsHandle handle) {
  return reinterpret_cast<uintptr_t>(pthread_getspecific(static_cast<pthread_key_t>(handle)));
}

bool SetTlsValue(TlsHandle handle, uintptr_t value) {
  return pthread_setspecific(static_cast<pthread_key_t>(handle), reinterpret_cast<void*>(value)) ==
         0;
}

constexpr auto kAlertablePollSlice = std::chrono::milliseconds(1);

// 35: a waiter that is about to sleep first spins briefly (it stays registered on the
// object, so Pulse / hand-over semantics are unchanged; see WaitLocked). The guest job and
// physics pools hand each other work items of 5-30 us through events and semaphores
// tens of thousands of times a second; a futex sleep + wake costs the waker a syscall
// and the sleeper 20-150 us before it runs again. REX_WAIT_SPIN_US sets the budget
// (0 = off), adapted per object: objects whose waits are long stop spinning.
uint32_t WaitSpinBudgetNs() {
  static const uint32_t budget = [] {
    uint32_t us = 30;
    if (const char* env = std::getenv("REX_WAIT_SPIN_US")) {
      char* end = nullptr;
      const unsigned long value = std::strtoul(env, &end, 10);
      if (end != env) us = uint32_t(std::min<unsigned long>(value, 1000));
    }
    return us * 1000u;
  }();
  return budget;
}

// 53: a spin budget of the calling thread (rex_thread_set_wait_spin, exported below).
// Main XThread wakes the RAGE job workers ~54 times a frame (888, batch 52, 06.11 20:30):
// each wake is a futex syscall on Main (notify_one in Semaphore::Release, 1.27 ms of Main's
// frame), and the woken worker runs 233 us later on average (p50 23 us, p90 580 us). 53 % of
// those sleeps last less than 200 us. A thread with its own budget keeps spinning within one
// wait until the budget is used up (not just once: with three workers on one semaphore, two
// lose every race), it is counted in spinners_, and a semaphore release that a spinning
// waiter will take skips the futex wake (Semaphore::ReleaseLocked). The adaptive spin_score_
// of the object still decides whether a wait spins at all (long waits stop spinning).
// UINT32_MAX: the global budget above. t_wait_spin_wfe: wait in WFE (the core stops until the
// line is written; armed only with the kernel's event stream, which bounds a WFE to ~100 us).
static thread_local uint32_t t_wait_spin_ns = UINT32_MAX;
static thread_local bool t_wait_spin_wfe = false;
// The budget applies only while the thread runs on one of the lowest-capacity CPUs (the
// little cluster): a spinner on a big core would hold a core that Main XThread, RenderGuest
// or audio want. Elsewhere the thread waits as any other (global budget, one spin).
static thread_local bool t_wait_spin_little_only = false;

// CPUs with the lowest cpu_capacity (sysfs, EAS). All CPUs when the table cannot be read or
// all capacities are equal.
static uint64_t LowCapacityCpuMask() {
  static const uint64_t mask = [] {
    int capacity[64] = {};
    int count = 0, lowest = 0;
    for (int cpu = 0; cpu < 64; ++cpu) {
      char path[96];
      std::snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpu_capacity", cpu);
      FILE* file = std::fopen(path, "r");
      if (!file) break;
      int value = 0;
      if (std::fscanf(file, "%d", &value) != 1) value = 0;
      std::fclose(file);
      capacity[cpu] = value;
      count = cpu + 1;
      if (value > 0 && (!lowest || value < lowest)) lowest = value;
    }
    if (!count || !lowest) return ~uint64_t(0);
    uint64_t low = 0;
    for (int cpu = 0; cpu < count; ++cpu)
      if (capacity[cpu] == lowest) low |= uint64_t(1) << cpu;
    return low;
  }();
  return mask;
}

inline uint32_t ThreadWaitSpinNs(bool* own) {
  const uint32_t ns = t_wait_spin_ns;
  *own = ns != UINT32_MAX;
  if (*own && t_wait_spin_little_only) {
    const int cpu = sched_getcpu();
    if (cpu >= 0 && cpu < 64 && !((LowCapacityCpuMask() >> cpu) & 1)) *own = false;
  }
  return *own ? ns : WaitSpinBudgetNs();
}

#if defined(__aarch64__)
// HWCAP_EVTSTRM: the generic timer's event stream is on (Linux arm64 default, 10 kHz).
static bool WfeEventStream() {
  static const bool on = [] {
#if defined(__linux__) || defined(__ANDROID__)
    constexpr unsigned long kHwcapEvtstrm = 1ul << 2;
    if (const char* env = std::getenv("REX_WAIT_SPIN_WFE"); env && env[0] == '0') return false;
    return (getauxval(AT_HWCAP) & kHwcapEvtstrm) != 0;
#else
    return false;
#endif
  }();
  return on;
}
inline uint64_t CounterTicks() {
  uint64_t value;
  asm volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value) : : "memory");
  return value;
}
inline uint64_t CounterFrequency() {
  uint64_t value;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(value));
  return value;
}
#endif

// 39: the self-healing wait slices of §38, measured. An infinite wait sleeps in
// slices (REX_WAIT_SAFETY_MS, default 20 ms as in §38). A slice that ends by
// timeout and finds the object already signalled is a wakeup that was lost: the
// thread was rescued by the slice, after up to one slice of stall. A non-zero
// "rescued" count means a notify path still drops wakeups; zero for a long run
// means the slice can be raised (each slice that expires wakes an idle thread:
// ~210 a second on the 888 trace of 30.10). Logged at most every 5 s.
static std::chrono::milliseconds WaitSafetySlice() {
  static const std::chrono::milliseconds slice = [] {
    unsigned long ms = 20;
    if (const char* env = std::getenv("REX_WAIT_SAFETY_MS")) {
      char* end = nullptr;
      const unsigned long value = std::strtoul(env, &end, 10);
      if (end != env && value >= 1) ms = std::min<unsigned long>(value, 5000);
    }
    return std::chrono::milliseconds(ms);
  }();
  return slice;
}

static std::atomic<uint64_t> g_wait_rescued_single{0};
// 41: a single-object rescue is split by when the object last changed. If the
// change (notify) happened more than 1 ms before the slice's timer expired, the
// waiter should have been woken by it and was not: a real lost wakeup ("lost").
// Otherwise the change raced with the timeout ("late": harmless, the 30.10 20:16
// run had ~6-11 rescues a second and could not tell the two apart).
static std::atomic<uint64_t> g_wait_lost_single{0};
static std::atomic<uint64_t> g_wait_lost_stall_us{0};
struct LostWaitName {
  std::atomic<uint64_t> key{0};  // first 8 bytes of the thread name
  std::atomic<uint32_t> count{0};
};
static LostWaitName g_wait_lost_names[8];
// 43: the 41/42 "lost" were mostly not lost. libc++'s condition_variable::wait_for decides
// timeout vs. no_timeout by elapsed time, not by how the futex wait ended: a waiter that
// WAS woken by the notify but got a CPU only after its slice had elapsed reports timeout.
// So a rescue is now split by whether a wake was issued for this object while the waiter
// slept (notify_seq_ moved): "lost" = no wake at all (a real bug), "late-run" = woken in
// time, but ran late (run-queue delay on a busy core; stall = that delay).
static std::atomic<uint64_t> g_wait_late_single{0};
static std::atomic<uint64_t> g_wait_late_stall_us{0};
static LostWaitName g_wait_late_names[8];
static std::atomic<uint64_t> g_wait_rescued_multi{0};
static std::atomic<uint64_t> g_wait_slices_expired{0};
static std::atomic<int64_t> g_wait_safety_last_log_ns{0};

static void MaybeLogWaitSafety() {
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t last = g_wait_safety_last_log_ns.load(std::memory_order_relaxed);
  if (now - last < 5'000'000'000ll) return;
  if (!g_wait_safety_last_log_ns.compare_exchange_strong(last, now, std::memory_order_relaxed))
    return;
  const uint64_t single = g_wait_rescued_single.exchange(0, std::memory_order_relaxed);
  const uint64_t lost = g_wait_lost_single.exchange(0, std::memory_order_relaxed);
  const uint64_t lost_stall_us = g_wait_lost_stall_us.exchange(0, std::memory_order_relaxed);
  char names[160];
  size_t used = 0;
  names[0] = '\0';
  for (LostWaitName& entry : g_wait_lost_names) {
    const uint32_t n = entry.count.exchange(0, std::memory_order_relaxed);
    const uint64_t key = entry.key.load(std::memory_order_relaxed);
    if (!n || !key || used + 24 >= sizeof(names)) continue;
    char name[9] = {};
    std::memcpy(name, &key, 8);
    used += size_t(std::snprintf(names + used, sizeof(names) - used, " %s=%u", name, n));
  }
  const uint64_t late = g_wait_late_single.exchange(0, std::memory_order_relaxed);
  const uint64_t late_stall_us = g_wait_late_stall_us.exchange(0, std::memory_order_relaxed);
  char late_names[160];
  size_t late_used = 0;
  late_names[0] = '\0';
  for (LostWaitName& entry : g_wait_late_names) {
    const uint32_t n = entry.count.exchange(0, std::memory_order_relaxed);
    const uint64_t key = entry.key.load(std::memory_order_relaxed);
    if (!n || !key || late_used + 24 >= sizeof(late_names)) continue;
    char name[9] = {};
    std::memcpy(name, &key, 8);
    late_used += size_t(
        std::snprintf(late_names + late_used, sizeof(late_names) - late_used, " %s=%u", name, n));
  }
  const uint64_t multi = g_wait_rescued_multi.exchange(0, std::memory_order_relaxed);
  const uint64_t slices = g_wait_slices_expired.exchange(0, std::memory_order_relaxed);
#if defined(__ANDROID__)
  __android_log_print(single || multi ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "LibertyRecomp",
                      "wait-safety: rescued single=%llu (lost=%llu stall=%.1fms:%s) "
                      "(late-run=%llu delay=%.1fms:%s) multi=%llu slices-expired=%llu "
                      "slice=%lldms (since last line)",
                      (unsigned long long)single, (unsigned long long)lost,
                      double(lost_stall_us) / 1000.0, lost ? names : " -",
                      (unsigned long long)late, double(late_stall_us) / 1000.0,
                      late ? late_names : " -", (unsigned long long)multi,
                      (unsigned long long)slices, (long long)WaitSafetySlice().count());
#else
  (void)single; (void)multi; (void)slices; (void)lost; (void)lost_stall_us; (void)names;
  (void)late; (void)late_stall_us; (void)late_names;
#endif
}

static void NoteSliceExpired() {
  g_wait_slices_expired.fetch_add(1, std::memory_order_relaxed);
  MaybeLogWaitSafety();
}

static void NoteRescued(bool multi) {
  (multi ? g_wait_rescued_multi : g_wait_rescued_single).fetch_add(1, std::memory_order_relaxed);
  MaybeLogWaitSafety();
}

static int64_t SteadyNowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// 41: rare (a rescue whose object changed well before the slice ended).
static void NoteLostWakeup(int64_t stall_ns, char kind, bool woken) {
  (woken ? g_wait_late_single : g_wait_lost_single).fetch_add(1, std::memory_order_relaxed);
  (woken ? g_wait_late_stall_us : g_wait_lost_stall_us)
      .fetch_add(uint64_t(std::max<int64_t>(stall_ns, 0) / 1000), std::memory_order_relaxed);
  LostWaitName* table = woken ? g_wait_late_names : g_wait_lost_names;
  char name[16] = {};
#if defined(__ANDROID__) || defined(__linux__)
  pthread_getname_np(pthread_self(), name, sizeof(name));
#endif
  // 42: "<first 6 chars of the thread name>/<object kind>".
  if (!name[0]) name[0] = '?';
  for (int i = 0; i < 6; ++i) if (!name[i]) name[i] = ' ';
  name[6] = '/';
  name[7] = kind;
  uint64_t key = 0;
  std::memcpy(&key, name, 8);
  for (LostWaitName* entry_ptr = table; entry_ptr != table + 8; ++entry_ptr) {
    LostWaitName& entry = *entry_ptr;
    uint64_t expected = 0;
    if (entry.key.load(std::memory_order_relaxed) == key ||
        entry.key.compare_exchange_strong(expected, key, std::memory_order_relaxed)) {
      entry.count.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
}

// 35: spin adaptation of WaitMultiple, per thread (the object set changes per call).
thread_local uint8_t t_multi_spin_score = 4;
thread_local uint32_t t_multi_spin_probe = 0;

inline void CpuRelax() {
#if defined(__aarch64__)
  asm volatile("isb" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#else
  std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Deadline of one wait call. milliseconds::max() means "no timeout"; very long timeouts
// are treated the same way so now() + timeout cannot overflow. The clock is read only
// when the wait really has to block: a wait satisfied at once, or a zero-timeout poll,
// never reads it.
class WaitDeadline {
 public:
  explicit WaitDeadline(std::chrono::milliseconds timeout) {
    constexpr auto kForever = std::chrono::hours(24 * 365 * 10);
    infinite_ = timeout == std::chrono::milliseconds::max() || timeout >= kForever;
    timeout_ = std::max(timeout, std::chrono::milliseconds::zero());
  }
  bool infinite() const { return infinite_; }
  bool zero() const { return !infinite_ && timeout_.count() == 0; }
  std::chrono::steady_clock::time_point at() const {
    if (!resolved_) {
      at_ = std::chrono::steady_clock::now() + timeout_;
      resolved_ = true;
    }
    return at_;
  }
  bool passed(std::chrono::steady_clock::time_point now) const {
    return !infinite_ && (zero() || now >= at());
  }

 private:
  bool infinite_ = true;
  std::chrono::milliseconds timeout_{0};
  mutable bool resolved_ = false;
  mutable std::chrono::steady_clock::time_point at_{};
};

// Wait semantics (what guest code expects from the 360 kernel, KeWaitFor*/KeSetEvent):
//  - a thread is registered on the object from the moment it starts waiting until it
//    returns, alertable waits included (they poll the APC flag without leaving the wait);
//  - SignalAndWait signals only after it holds the wait object's lock, so the thread it
//    releases cannot act on the wait object before this thread is registered there;
//  - KeSetEvent on a manual-reset event releases every thread waiting at that moment,
//    even if KeResetEvent follows at once (Set/Reset "pulse");
//  - KeSetEvent on an auto-reset event with waiters releases one of THOSE waiters: two
//    Sets release two of them, and a thread that starts waiting later cannot take it.
// Lock order: handle mutex_ (several only in address order or std::lock) -> waiter mutex.
class PosixConditionBase {
 public:
  PosixConditionBase() {
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID  // Bionic has no robust mutexes
    // Use robust mutexes so waits can recover if owner thread terminates.
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) == 0) {
      if (pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0) {
        auto native_mutex = static_cast<pthread_mutex_t*>(mutex_.native_handle());
        pthread_mutex_destroy(native_mutex);
        pthread_mutex_init(native_mutex, &attr);
      }
      pthread_mutexattr_destroy(&attr);
    }
#endif
  }

  virtual ~PosixConditionBase() = default;

  // Signal() takes mutex_; SignalLocked() expects the caller to hold it (SignalAndWait).
  // 35: the condition-variable wake happens after mutex_ is released (DeferredNotify).
  // Notifying under the lock made every woken waiter block on mutex_ right away (bionic
  // wakes it with FUTEX_WAKE, no requeue) and the setter's unlock then had to wake it a
  // second time: two sleeps / two syscalls per Set in the job and physics pools.
  virtual bool Signal() {
    DeferredNotify deferred;
    bool result;
    {
      if (!LockMutex(mutex_)) {
        return false;
      }
      std::lock_guard<std::mutex> lock(mutex_, std::adopt_lock);
      deferred_ = &deferred;
      result = SignalLocked();
      deferred_ = nullptr;
      FireDeferredUnderLock(deferred);  // 42
    }
    FireDeferred(deferred);
    return result;
  }
  virtual bool SignalLocked() = 0;

  WaitResult Wait(std::chrono::milliseconds timeout) {
    return WaitUntil(nullptr, WaitDeadline(timeout), nullptr);
  }

  // Waits for this object until `deadline`.
  //  to_signal  - signalled first (SignalAndWait), atomically with the registration here:
  //               both locks are taken in address order, so SignalAndWait(A, B) racing
  //               SignalAndWait(B, A) cannot deadlock.
  //  alert_flag - alertable wait: returns kUserCallback once the flag is set. The thread
  //               stays registered while it polls the flag every kAlertablePollSlice.
  WaitResult WaitUntil(PosixConditionBase* to_signal, const WaitDeadline& deadline,
                       const std::atomic<bool>* alert_flag) {
    if (to_signal && to_signal != this) {
      PosixConditionBase* first = std::less<PosixConditionBase*>()(this, to_signal) ? this : to_signal;
      PosixConditionBase* second = first == this ? to_signal : this;
      if (!LockMutex(first->mutex_)) {
        return WaitResult::kFailed;
      }
      if (!LockMutex(second->mutex_)) {
        first->mutex_.unlock();
        return WaitResult::kFailed;
      }
      const bool signaled_ok = to_signal->SignalLocked();
      to_signal->mutex_.unlock();
      std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
      if (!signaled_ok) {
        return WaitResult::kFailed;
      }
      return WaitLocked(lock, deadline, alert_flag);
    }
    if (!LockMutex(mutex_)) {
      return WaitResult::kFailed;
    }
    std::unique_lock<std::mutex> lock(mutex_, std::adopt_lock);
    if (to_signal && !SignalLocked()) {
      return WaitResult::kFailed;
    }
    return WaitLocked(lock, deadline, alert_flag);
  }

  static std::pair<WaitResult, size_t> WaitMultiple(std::vector<PosixConditionBase*>&& handles,
                                                    bool wait_all,
                                                    std::chrono::milliseconds timeout) {
    return WaitMultipleUntil(handles, wait_all, WaitDeadline(timeout), nullptr);
  }

  static std::pair<WaitResult, size_t> WaitMultipleUntil(
      const std::vector<PosixConditionBase*>& handles, bool wait_all,
      const WaitDeadline& deadline, const std::atomic<bool>* alert_flag) {
    assert_true(!handles.empty());

    if (handles.size() == 1) {
      auto result = handles[0]->WaitUntil(nullptr, deadline, alert_flag);
      return std::make_pair(result, size_t(0));
    }

    // Every handle wakes this waiter on Signal, so the thread sleeps until a
    // handle changes instead of polling (polling burned whole cores on
    // Android: the audio worker alone spun ~40% of a big core here).
    // Wait-any also takes a ticket on every handle, like a single wait, so a manual-reset
    // pulse or an auto-reset Set handed to the waiters reaches it. Wait-all keeps the plain
    // state check: it is satisfied only when all objects are signalled at the same time.
    ExternalWaiter waiter;
    std::vector<WaitTicket> tickets(handles.size());
    std::vector<bool> consumed(handles.size(), false);
    for (size_t i = 0; i < handles.size(); ++i) {
      std::lock_guard<std::mutex> lock(handles[i]->mutex_);
      handles[i]->external_waiters_.push_back(&waiter);
      if (!wait_all) {
        tickets[i] = handles[i]->EnterWaitLocked();
      }
    }
    struct Unregister {
      const std::vector<PosixConditionBase*>& handles;
      ExternalWaiter* waiter;
      bool wait_all;
      const std::vector<WaitTicket>& tickets;
      const std::vector<bool>& consumed;
      ~Unregister() {
        for (size_t i = 0; i < handles.size(); ++i) {
          std::lock_guard<std::mutex> lock(handles[i]->mutex_);
          auto& list = handles[i]->external_waiters_;
          list.erase(std::remove(list.begin(), list.end(), waiter), list.end());
          if (!wait_all) {
            handles[i]->ExitWaitLocked(tickets[i], consumed[i]);
          }
        }
      }
    } unregister{handles, &waiter, wait_all, tickets, consumed};

    const uint32_t spin_ns = WaitSpinBudgetNs();
    bool spin_pending = spin_ns && (t_multi_spin_score >= 4 || (++t_multi_spin_probe & 31u) == 0);
    bool spun = false;
    bool slice_expired = false;  // 39: the last sleep ended by the safety slice
    while (true) {
      uint64_t generation;
      {
        std::lock_guard<std::mutex> lock(waiter.mutex);
        generation = waiter.generation;
      }

      if (wait_all) {
        // Acquire every handle atomically so the state cannot change between
        // the check and the consumption.
        std::vector<std::unique_lock<std::mutex>> locks;
        locks.reserve(handles.size());
        for (auto* handle : handles) {
          locks.emplace_back(handle->mutex_, std::defer_lock);
        }
        if (locks.size() == 2) {
          std::lock(locks[0], locks[1]);
        } else {
          // std::lock is variadic only; lock in address order instead.
          std::vector<size_t> order(locks.size());
          for (size_t i = 0; i < order.size(); ++i) order[i] = i;
          std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return std::less<PosixConditionBase*>()(handles[a], handles[b]);
          });
          for (size_t index : order) {
            locks[index].lock();
          }
        }
        bool all_signaled = true;
        for (auto* handle : handles) {
          if (!handle->signaled()) {
            all_signaled = false;
            break;
          }
        }
        if (all_signaled) {
          for (auto* handle : handles) {
            handle->post_execution();
          }
          if (spun) t_multi_spin_score = uint8_t(std::min<uint32_t>(t_multi_spin_score + 2u, 16u));
          if (slice_expired) NoteRescued(true);
          return std::make_pair(WaitResult::kSuccess, size_t(0));
        }
      } else {
        for (size_t i = 0; i < handles.size(); ++i) {
          std::lock_guard<std::mutex> lock(handles[i]->mutex_);
          if (handles[i]->ReadyLocked(tickets[i])) {
            handles[i]->ConsumeLocked(tickets[i]);
            consumed[i] = true;
            if (spun) t_multi_spin_score = uint8_t(std::min<uint32_t>(t_multi_spin_score + 2u, 16u));
            if (slice_expired) NoteRescued(true);
            return std::make_pair(WaitResult::kSuccess, i);
          }
        }
      }

      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
        return std::make_pair(WaitResult::kUserCallback, size_t(0));
      }
      if (deadline.zero()) {
        return std::make_pair(WaitResult::kTimeout, size_t(0));
      }
      const auto now = std::chrono::steady_clock::now();
      if (deadline.passed(now)) {
        return std::make_pair(WaitResult::kTimeout, size_t(0));
      }

      // 35: spin once before the first sleep, as single waits do (WaitLocked); the
      // thread's recent multi-waits decide (t_multi_spin_score). Main XThread waits for
      // the job pool's completion events this way.
      if (spin_pending) {
        spin_pending = false;
        spun = true;
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t i = 1; waiter.published.load(std::memory_order_acquire) == generation; ++i) {
          if (alert_flag && alert_flag->load(std::memory_order_relaxed)) break;
          CpuRelax();
          if ((i & 15u) == 0 &&
              std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(spin_ns))
            break;
        }
        if (waiter.published.load(std::memory_order_acquire) != generation) continue;
      }
      if (spun) {
        spun = false;
        t_multi_spin_score = t_multi_spin_score ? uint8_t(t_multi_spin_score - 1) : 0;
      }
      std::unique_lock<std::mutex> lock(waiter.mutex);
      if (waiter.generation != generation) {
        continue;
      }
      auto changed = [&] { return waiter.generation != generation; };
      waiter.sleeping = true;
      slice_expired = false;
      if (alert_flag) {
        auto until = now + kAlertablePollSlice;
        if (!deadline.infinite() && deadline.at() < until) until = deadline.at();
        waiter.cond.wait_until(lock, until, changed);
      } else if (deadline.infinite()) {
        slice_expired = !waiter.cond.wait_for(lock, WaitSafetySlice(), changed);
      } else {
        auto until = deadline.at();
        auto slice = now + WaitSafetySlice();
        slice_expired = !waiter.cond.wait_until(lock, std::min(until, slice), changed) &&
                        slice < until;
      }
      waiter.sleeping = false;
      if (slice_expired) NoteSliceExpired();
    }
  }

  struct ExternalWaiter {
    std::mutex mutex;
    std::condition_variable cond;
    uint64_t generation = 0;
    // 35: `generation` for spinning without the mutex, and whether the waiter is
    // inside cond.wait*: a notify skips the futex wake while it is not (bionic's
    // broadcast is a syscall even with nobody waiting; every job-completion Set paid it).
    std::atomic<uint64_t> published{0};
    bool sleeping = false;  // under mutex
  };

  // Callers hold mutex_. Wakes every WaitMultiple currently including this
  // handle; lock order is always handle mutex_ then waiter mutex.
  void NotifyExternalWaiters() {
    for (ExternalWaiter* waiter : external_waiters_) {
      std::lock_guard<std::mutex> lock(waiter->mutex);
      ++waiter->generation;
      waiter->published.store(waiter->generation, std::memory_order_release);
      waiter->cond.notify_all();
    }
  }

  virtual void* native_handle() const {
    return const_cast<std::condition_variable&>(cond_).native_handle();
  }

 protected:
  // A waiter's registration on one object (kept by the waiting thread, used under mutex_).
  struct WaitTicket {
    uint64_t seen = 0;  // the object's signal counter when the wait started
  };

  // Locks a handle mutex; on desktop Linux the mutex is robust and is recovered
  // if its owner died holding it.
  static bool LockMutex(std::mutex& mutex) {
#if REX_PLATFORM_LINUX && !REX_PLATFORM_ANDROID  // Bionic has no robust mutexes
    auto native_mutex = static_cast<pthread_mutex_t*>(mutex.native_handle());
    int lock_result = pthread_mutex_lock(native_mutex);
    if (lock_result == EOWNERDEAD) {
      pthread_mutex_consistent(native_mutex);
      return true;
    }
    return lock_result == 0;
#else
    mutex.lock();
    return true;
#endif
  }

  WaitResult WaitLocked(std::unique_lock<std::mutex>& lock, const WaitDeadline& deadline,
                        const std::atomic<bool>* alert_flag) {
    // Fast path: already signalled (or a zero-timeout poll) needs no registration.
    {
      const WaitTicket probe = PeekTicketLocked();
      if (ReadyLocked(probe)) {
        ConsumeLocked(probe);
        return WaitResult::kSuccess;
      }
      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
        return WaitResult::kUserCallback;
      }
      if (deadline.zero()) {
        return WaitResult::kTimeout;
      }
    }
    const WaitTicket ticket = EnterWaitLocked();
    WaitResult result;
    // 35: one spin per wait, before the first sleep, while this object's recent waits
    // were short (spin_score_). The thread stays registered (ticket above), so a Set or
    // Pulse during the spin is seen exactly as by a sleeping waiter; it is just not
    // counted in cond_waiters_, so the setter skips the futex wake.
    bool own_spin = false;  // 53: the thread's own budget (rex_thread_set_wait_spin)
    const uint32_t spin_ns = ThreadWaitSpinNs(&own_spin);
    // 54: a thread with its own budget spins on every wait. The object's score did not fit
    // it: three job workers wait on one semaphore, a release goes to one of them and the
    // other two count a failure, so the score sank below kSpinScoreMin and only every 32nd
    // wait spun (888, batch 53 N1: the workers' sleeps ended by Main XThread stayed at ~56 a
    // frame, as with spin=0; spinning took 0.5 ms a frame instead of ~6). The budget itself
    // bounds the cost: at most spin_ns per wait.
    bool spin_pending =
        spin_ns && (own_spin || spin_score_ >= kSpinScoreMin || (++spin_probe_ & kSpinProbeMask) == 0);
    int64_t spin_until_ns = 0;  // 53: end of this wait's budget (own budget only)
    bool spun = false;
    bool slice_expired = false;  // 39: the last sleep ended by the safety slice
    int64_t slice_end_ns = 0;    // 41: when that slice's timer was due
    uint64_t notify_seq_at_sleep = 0;  // 43: wakes issued for this object before the sleep
    while (true) {
      // Checked before the alert flag and the deadline: a wait that was satisfied wins.
      if (ReadyLocked(ticket)) {
        ConsumeLocked(ticket);
        result = WaitResult::kSuccess;
        if (spun) spin_score_ = uint8_t(std::min<uint32_t>(spin_score_ + 2u, kSpinScoreMax));
        if (slice_expired) {
          NoteRescued(false);
          // 41: changed > 1 ms before the slice ended, yet the notify did not wake us.
          if (last_change_ns_ && last_change_ns_ + 1'000'000 < slice_end_ns) {
            NoteLostWakeup(SteadyNowNs() - last_change_ns_, KindTag(),
                           notify_seq_ != notify_seq_at_sleep);
          }
        }
        break;
      }
      if (alert_flag && alert_flag->load(std::memory_order_acquire)) {
        result = WaitResult::kUserCallback;
        break;
      }
      const auto now = std::chrono::steady_clock::now();
      if (deadline.passed(now)) {
        result = WaitResult::kTimeout;
        break;
      }
      if (spin_pending) {
        spin_pending = false;
        spun = true;
        uint32_t budget_ns = spin_ns;
        if (own_spin) {
          // 53: one budget per wait, spent over as many spins as it takes.
          const int64_t now_ns = SteadyNowNs();
          if (!spin_until_ns) spin_until_ns = now_ns + int64_t(spin_ns);
          budget_ns = spin_until_ns > now_ns ? uint32_t(spin_until_ns - now_ns) : 0;
          ++spinners_;  // under mutex_: a release sees this waiter as one that takes a count
        }
        const uint32_t seen = epoch_.load(std::memory_order_relaxed);
        lock.unlock();
        SpinUntilEpochMoves(seen, budget_ns, alert_flag, own_spin && t_wait_spin_wfe);
        // The setter may still hold mutex_ for a moment; take it without parking on
        // the futex (which would make the setter's unlock a wake syscall).
        bool locked = false;
        for (int attempt = 0; attempt < 64 && !(locked = lock.try_lock()); ++attempt) CpuRelax();
        if (!locked) lock.lock();
        if (own_spin) {
          --spinners_;
          // Another waiter took the change, or it was not one for this ticket: spin on while
          // the budget lasts (checked again at the top: ready, alert, deadline).
          if (!ReadyLocked(ticket) && SteadyNowNs() + 2'000 < spin_until_ns) spin_pending = true;
        }
        continue;
      }
      if (spun) {
        // Spun and still not ready: this object's waits are long.
        spun = false;
        spin_score_ = spin_score_ ? uint8_t(spin_score_ - 1) : 0;
      }
      // cond_waiters_ is changed only under mutex_, around the wait itself: a Set that
      // takes mutex_ after this point sees the waiter and broadcasts (no lost wakeup).
      ++cond_waiters_;
      slice_expired = false;
      notify_seq_at_sleep = notify_seq_;
      if (alert_flag) {
        auto until = now + kAlertablePollSlice;
        if (!deadline.infinite() && deadline.at() < until) until = deadline.at();
        cond_.wait_until(lock, until);
      } else if (deadline.infinite()) {
        slice_end_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           (now + WaitSafetySlice()).time_since_epoch())
                           .count();
        slice_expired = cond_.wait_for(lock, WaitSafetySlice()) == std::cv_status::timeout;
      } else {
        auto until = deadline.at();
        auto slice = now + WaitSafetySlice();
        slice_end_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(slice.time_since_epoch()).count();
        slice_expired = cond_.wait_until(lock, std::min(until, slice)) == std::cv_status::timeout &&
                        slice < until;
      }
      --cond_waiters_;
      if (slice_expired) NoteSliceExpired();
    }
    ExitWaitLocked(ticket, result == WaitResult::kSuccess);
    // 35: wakes are targeted now (one sleeper per released count / handed release). A
    // sleeper picked for one that leaves without taking it (timeout, alert) passes it on.
    // 41: with several sleepers, notify_one may pick one whose ticket cannot take what is
    // passed on (auto-reset grants are per ticket), as §38 found for Set: wake them all.
    if (result != WaitResult::kSuccess && cond_waiters_ && PassOnLocked()) {
      ++notify_seq_;  // 43
      if (cond_waiters_ > 1) {
        cond_.notify_all();
      } else {
        cond_.notify_one();
      }
    }
    return result;
  }

  // Ticket hooks, all called with mutex_ held. The defaults are the plain state check;
  // Event overrides them for the pulse / hand-over rules above.
  virtual WaitTicket EnterWaitLocked() { return {}; }
  // The ticket a wait would get now, without registering (first check of a wait).
  virtual WaitTicket PeekTicketLocked() const { return {}; }
  virtual bool ReadyLocked(const WaitTicket&) const { return signaled(); }
  virtual void ConsumeLocked(const WaitTicket&) { post_execution(); }
  virtual void ExitWaitLocked(const WaitTicket&, bool /*consumed*/) {}
  // 35: another waiter could take what this object holds now (pass-on after a timeout).
  virtual bool PassOnLocked() const { return signaled(); }

  inline virtual bool signaled() const = 0;
  inline virtual void post_execution() = 0;
  // 42: object kind for the lost-wakeup line (E auto event, N manual event, S semaphore,
  // M mutant, T timer, H thread).
  virtual char KindTag() const { return '?'; }

  // Callers hold mutex_. Wakes the threads blocked on this object. bionic's
  // pthread_cond_broadcast is a futex syscall even when nobody waits (job and
  // physics threads Set events thousands of times a second), so the broadcast
  // is skipped when no thread is inside cond_.wait*.
  // 35: `wake` says how many sleepers the change can satisfy: kWakeAll (manual reset,
  // a timer) or a count (one per released semaphore count / handed release). Called
  // inside Signal() / Release() / Pulse() the futex wake is deferred to after the
  // unlock (deferred_); elsewhere it happens here, as before. epoch_ lets spinning
  // waiters see the change without the lock.
  static constexpr uint32_t kWakeAll = UINT32_MAX;
  void NotifyWaitersLocked(uint32_t wake = kWakeAll) {
    epoch_.fetch_add(1, std::memory_order_release);
    // 41: for the lost-wakeup check in WaitLocked; only needed when someone sleeps.
    if (cond_waiters_) last_change_ns_ = SteadyNowNs();
    if (cond_waiters_ && wake) ++notify_seq_;  // 43: a futex wake follows (now or deferred)
    if (cond_waiters_ && wake) {
      if (deferred_) {
        if (wake == kWakeAll || deferred_->all) {
          deferred_->all = true;
        } else {
          deferred_->ones = std::min<uint32_t>(deferred_->ones + wake, cond_waiters_);
        }
      } else if (wake == kWakeAll || wake >= cond_waiters_) {
        cond_.notify_all();
      } else {
        for (uint32_t i = 0; i < wake; ++i) cond_.notify_one();
      }
    }
    NotifyExternalWaiters();
  }
  void BumpEpochLocked() { epoch_.fetch_add(1, std::memory_order_release); }

  struct DeferredNotify {
    bool all = false;
    uint32_t ones = 0;
  };
  // 42: the futex wake now happens before mutex_ is released (classic order). The 41 run
  // (collisions, 888) measured "lost" wakeups: the object changed 2-7 ms (avg) before the
  // waiter's slice ended, mostly RagePhysics. With the wake deferred past the unlock, the
  // unlock itself can wake a contender for mutex_ that preempts the releaser on its core
  // (nice 0 worker vs. nice -2..-8 waiters) before the cond wake is issued; the sleeper
  // then waits for the releaser to run again, or for its 20 ms slice. Waking under the
  // lock closes that window; the woken thread may block briefly on mutex_ instead.
  // REX_WAIT_DEFER_NOTIFY=1 restores the 35 order (after the unlock) for A/B.
  static bool NotifyUnderLock() {
    static const bool under_lock = [] {
      const char* env = std::getenv("REX_WAIT_DEFER_NOTIFY");
      return !(env && env[0] == '1');
    }();
    return under_lock;
  }
  void FireDeferredUnderLock(DeferredNotify& deferred) {
    if (!NotifyUnderLock()) return;
    FireDeferred(deferred);
    deferred = DeferredNotify{};
  }
  // After mutex_ is released (or under it, see above). The caller (Set / Release /
  // Pulse) keeps the object alive.
  void FireDeferred(const DeferredNotify& deferred) {
    if (deferred.all) {
      cond_.notify_all();
    } else {
      for (uint32_t i = 0; i < deferred.ones; ++i) cond_.notify_one();
    }
  }

  void SpinUntilEpochMoves(uint32_t seen, uint32_t spin_ns, const std::atomic<bool>* alert_flag,
                           bool wfe = false) const {
#if defined(__aarch64__)
    // 53: WFE instead of a busy loop. LDAXR arms the exclusive monitor on epoch_'s line; a
    // store to that line from another core (the setter's epoch_ bump, but also its mutex_)
    // clears it and wakes the WFE. The event stream ends a WFE at least every ~100 us, so the
    // budget is kept to about that; without the stream WFE could sleep until an interrupt
    // and is not used. The core is clock-gated meanwhile instead of issuing isb.
    if (wfe && WfeEventStream()) {
      static const uint64_t frequency = CounterFrequency();
      const uint64_t start = CounterTicks();
      const uint64_t ticks = frequency ? uint64_t(spin_ns) * frequency / 1'000'000'000ull : 0;
      const auto* word = reinterpret_cast<const uint32_t*>(&epoch_);
      static_assert(sizeof(epoch_) == sizeof(uint32_t));
      while (true) {
        uint32_t value;
        asm volatile("ldaxr %w0, [%1]" : "=&r"(value) : "r"(word) : "memory");
        if (value != seen || (alert_flag && alert_flag->load(std::memory_order_relaxed)) ||
            CounterTicks() - start >= ticks) {
          asm volatile("clrex" ::: "memory");
          return;
        }
        asm volatile("wfe" ::: "memory");
      }
    }
#else
    (void)wfe;
#endif
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t i = 1;; ++i) {
      if (epoch_.load(std::memory_order_acquire) != seen) return;
      if (alert_flag && alert_flag->load(std::memory_order_relaxed)) return;
      CpuRelax();
      if ((i & 15u) == 0 &&
          std::chrono::steady_clock::now() - start >= std::chrono::nanoseconds(spin_ns))
        return;
    }
  }

  std::condition_variable cond_;
  std::mutex mutex_;
  uint32_t cond_waiters_ = 0;                      // threads in cond_.wait*, guarded by mutex_
  int64_t last_change_ns_ = 0;                     // 41: last notify with sleepers, mutex_
  uint64_t notify_seq_ = 0;                        // 43: wakes issued, mutex_
  std::vector<ExternalWaiter*> external_waiters_;  // guarded by mutex_
  DeferredNotify* deferred_ = nullptr;             // guarded by mutex_ (Signal/Release/Pulse)
  std::atomic<uint32_t> epoch_{0};                 // bumped by every notify (spinners)
  // Spin adaptation, guarded by mutex_: +2 when a spin caught the release, -1 when the
  // wait slept anyway. Below kSpinScoreMin only every 32nd wait spins (a probe).
  static constexpr uint8_t kSpinScoreMin = 4, kSpinScoreMax = 16;
  static constexpr uint32_t kSpinProbeMask = 31;
  uint8_t spin_score_ = kSpinScoreMin;
  uint32_t spin_probe_ = 0;
  // 53: threads spinning in WaitLocked on their own budget (rex_thread_set_wait_spin), under
  // mutex_. Each one re-checks the object under mutex_ before it sleeps, so a semaphore count
  // that one of them will take needs no futex wake (Semaphore::ReleaseLocked).
  uint32_t spinners_ = 0;
};

// There really is no native POSIX handle for a single wait/signal construct
// pthreads is at a lower level with more handles for such a mechanism.
// This simple wrapper class functions as our handle and uses conditional
// variables for waits and signals.
template <typename T>
class PosixCondition {};

template <>
class PosixCondition<Event> : public PosixConditionBase {
 public:
  char KindTag() const override { return manual_reset_ ? 'N' : 'E'; }  // 42
  PosixCondition(bool manual_reset, bool initial_state)
      : signal_(initial_state), manual_reset_(manual_reset) {}
  virtual ~PosixCondition() = default;

  bool SignalLocked() override {
    ++set_count_;
    if (manual_reset_) {
      // Waiters registered before this Set see set_count_ move past their ticket and
      // return even if Reset() follows at once.
      signal_ = true;
    } else if (signal_) {
      // Already signalled: a second Set changes nothing (as on the 360).
    } else if (waiter_seen_.size() > grants_.size()) {
      // Hand the release to a thread that is already waiting (one per Set).
      grants_.push_back(set_count_);
    } else {
      signal_ = true;
    }
    // 35: an auto-reset Set satisfies one waiter; if multiple threads are waiting on
    // cond_, notify_all ensures the eligible waiter is woken rather than a random one.
    NotifyWaitersLocked(manual_reset_ ? kWakeAll : (cond_waiters_ > 1 ? kWakeAll : 1));
    return true;
  }

  void Reset() {
    auto lock = std::unique_lock<std::mutex>(mutex_);
    signal_ = false;  // releases already handed to waiters stay theirs
  }

  // KePulseEvent: satisfy the waits in progress (all for manual reset, one for auto
  // reset), then leave the event non-signalled.
  void Pulse() {
    DeferredNotify deferred;
    {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      deferred_ = &deferred;
      if (manual_reset_) {
        ++set_count_;
      } else if (!signal_ && waiter_seen_.size() > grants_.size()) {
        ++set_count_;
        grants_.push_back(set_count_);
      }
      signal_ = false;
      // 41: as SignalLocked (§38): the grant belongs to one ticket, notify_one may wake
      // another sleeper.
      NotifyWaitersLocked(manual_reset_ ? kWakeAll : (cond_waiters_ > 1 ? kWakeAll : 1));
      deferred_ = nullptr;
      FireDeferredUnderLock(deferred);  // 42
    }
    FireDeferred(deferred);
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }

  WaitTicket PeekTicketLocked() const override {
    WaitTicket ticket;
    ticket.seen = set_count_;
    return ticket;
  }
  WaitTicket EnterWaitLocked() override {
    WaitTicket ticket;
    ticket.seen = set_count_;
    waiter_seen_.push_back(ticket.seen);  // stays sorted: set_count_ only grows
    return ticket;
  }
  bool PassOnLocked() const override { return !manual_reset_ && (signal_ || !grants_.empty()); }
  bool ReadyLocked(const WaitTicket& ticket) const override {
    if (signal_) return true;
    if (manual_reset_) return set_count_ > ticket.seen;
    // grants_ is sorted, so the newest decides whether any was made after we started.
    return !grants_.empty() && grants_.back() > ticket.seen;
  }
  void ConsumeLocked(const WaitTicket& ticket) override {
    if (manual_reset_) {
      return;
    }
    // A release handed to the waiters first (it is ours to take); the oldest one we may
    // take, so later waiters keep the newer ones.
    for (auto it = grants_.begin(); it != grants_.end(); ++it) {
      if (*it > ticket.seen) {
        grants_.erase(it);
        return;
      }
    }
    signal_ = false;
  }
  void ExitWaitLocked(const WaitTicket& ticket, bool /*consumed*/) override {
    auto it = std::find(waiter_seen_.begin(), waiter_seen_.end(), ticket.seen);
    assert_true(it != waiter_seen_.end());
    if (it != waiter_seen_.end()) {
      waiter_seen_.erase(it);
    }
    if (manual_reset_ || grants_.empty()) {
      return;
    }
    // A release only waiters that started before it may take. If the ones left cannot
    // take them all (a wait-any that returned on another object), the rest becomes a
    // plain signal so it is not lost. Eligible sets are nested (earlier waiters may take
    // every later release), so the greedy count below is a maximum matching.
    bool orphaned = false;
    size_t kept = 0, eligible = 0;
    for (auto grant = grants_.begin(); grant != grants_.end();) {
      while (eligible < waiter_seen_.size() && waiter_seen_[eligible] < *grant) ++eligible;
      if (eligible > kept) {
        ++kept;
        ++grant;
      } else {
        grant = grants_.erase(grant);
        orphaned = true;
      }
    }
    if (orphaned && !signal_) {
      signal_ = true;
      NotifyWaitersLocked();
    }
  }

  bool signal_;
  const bool manual_reset_;
  uint64_t set_count_ = 0;              // every Set/Pulse that released waiters
  std::vector<uint64_t> waiter_seen_;   // tickets of the threads waiting now (sorted)
  std::deque<uint64_t> grants_;         // auto reset: releases handed to them (sorted)
};

template <>
class PosixCondition<Semaphore> : public PosixConditionBase {
 public:
  char KindTag() const override { return 'S'; }  // 42
  PosixCondition(uint32_t initial_count, uint32_t maximum_count)
      : count_(initial_count), maximum_count_(maximum_count) {}

  bool SignalLocked() override { return ReleaseLocked(1, nullptr); }

  bool Release(uint32_t release_count, int* out_previous_count) {
    DeferredNotify deferred;
    bool result;
    {
      // 53: a spinning waiter holds mutex_ for a moment after it saw the change (re-check
      // and take the count); wait that out without parking on the futex, as the spinner
      // itself does. Main XThread releases the job semaphore ~54 times a frame.
      auto lock = std::unique_lock<std::mutex>(mutex_, std::try_to_lock);
      for (int attempt = 0; attempt < 64 && !lock.owns_lock(); ++attempt) {
        CpuRelax();
        if (lock.try_lock()) break;
      }
      if (!lock.owns_lock()) lock.lock();
      deferred_ = &deferred;
      result = ReleaseLocked(release_count, out_previous_count);
      deferred_ = nullptr;
      FireDeferredUnderLock(deferred);  // 42
    }
    FireDeferred(deferred);
    return result;
  }

  bool ReleaseLocked(uint32_t release_count, int* out_previous_count) {
    if (release_count > maximum_count_ - count_) {
      return false;
    }
    if (out_previous_count) {
      *out_previous_count = count_;
    }
    const uint32_t before = count_;
    count_ += release_count;
    // 35: one sleeper per released count. notify_all woke every worker of the guest job
    // pool for each job; all but one went straight back to sleep (77 % of the job-thread
    // wake-ups on the 8 Elite Gen 5 trace ran < 15 us).
    // 53: minus the counts that spinning waiters will take. Spinners take the counts that
    // were already there first, so only spinners beyond `before` are free for new ones. A
    // spinner always re-checks under mutex_ before it sleeps, and one that leaves without
    // a count (timeout, alert) passes the count on (WaitLocked), so nothing is stranded.
    const uint32_t free_spinners = spinners_ > before ? spinners_ - before : 0;
    NotifyWaitersLocked(release_count - std::min(release_count, free_spinners));
    return true;
  }

 private:
  inline bool signaled() const override { return count_ > 0; }
  inline void post_execution() override {
    // A consumed count never makes another waiter ready (WaitAll included), so no
    // broadcast here: it was a futex syscall per successful wait.
    count_--;
  }
  uint32_t count_;
  const uint32_t maximum_count_;
};

template <>
class PosixCondition<Mutant> : public PosixConditionBase {
 public:
  char KindTag() const override { return 'M'; }  // 42
  explicit PosixCondition(bool initial_owner) : count_(0) {
    if (initial_owner) {
      count_ = 1;
      owner_ = std::this_thread::get_id();
    }
  }

  bool SignalLocked() override { return ReleaseLocked(); }

  bool Release() {
    DeferredNotify deferred;
    bool result;
    {
      auto lock = std::unique_lock<std::mutex>(mutex_);
      deferred_ = &deferred;
      result = ReleaseLocked();
      deferred_ = nullptr;
      FireDeferredUnderLock(deferred);  // 42
    }
    FireDeferred(deferred);
    return result;
  }

  // owner_ and count_ are read under mutex_ (they were read before taking it).
  bool ReleaseLocked() {
    if (owner_ == std::this_thread::get_id() && count_ > 0) {
      --count_;
      // Free to be acquired by another thread (one of them).
      if (count_ == 0) {
        NotifyWaitersLocked(1);
      }
      return true;
    }
    return false;
  }

  void* native_handle() const override { return const_cast<std::mutex&>(mutex_).native_handle(); }

 private:
  inline bool signaled() const override {
    return count_ == 0 || owner_ == std::this_thread::get_id();
  }
  inline void post_execution() override {
    count_++;
    owner_ = std::this_thread::get_id();
  }
  uint32_t count_;
  std::thread::id owner_;
};

template <>
class PosixCondition<Timer> : public PosixConditionBase {
 public:
  char KindTag() const override { return 'T'; }  // 42
  explicit PosixCondition(bool manual_reset)
      : callback_(nullptr), signal_(false), manual_reset_(manual_reset) {}

  virtual ~PosixCondition() { Cancel(); }

  bool SignalLocked() override {
    signal_ = true;
    NotifyWaitersLocked();
    return true;
  }

  void SetOnce(std::chrono::steady_clock::time_point due_time, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerOnce(&CompletionRoutine, this, due_time);
  }

  void SetRepeating(std::chrono::steady_clock::time_point due_time,
                    std::chrono::milliseconds period, std::function<void()> opt_callback) {
    Cancel();

    std::lock_guard<std::mutex> lock(mutex_);

    callback_ = std::move(opt_callback);
    signal_ = false;
    wait_item_ = QueueTimerRecurring(&CompletionRoutine, this, due_time, period);
  }

  void Cancel() {
    if (auto wait_item = wait_item_.lock()) {
      wait_item->Disarm();
    }
  }

  void* native_handle() const override {
    assert_always();
    return nullptr;
  }

 private:
  static void CompletionRoutine(void* userdata) {
    assert_not_null(userdata);
    auto timer = reinterpret_cast<PosixCondition<Timer>*>(userdata);
    timer->Signal();
    // As the callback may reset the timer, store local.
    std::function<void()> callback;
    {
      std::lock_guard<std::mutex> lock(timer->mutex_);
      callback = timer->callback_;
    }
    if (callback) {
      callback();
    }
  }

 private:
  inline bool signaled() const override { return signal_; }
  inline void post_execution() override {
    if (!manual_reset_) {
      signal_ = false;
    }
  }
  std::weak_ptr<TimerQueueWaitItem> wait_item_;
  std::function<void()> callback_;
  bool signal_;  // Protected by mutex_
  const bool manual_reset_;
};

struct ThreadStartData {
  std::function<void()> start_routine;
  bool create_suspended;
  Thread* thread_obj;
};

template <>
class PosixCondition<Thread> : public PosixConditionBase {
  enum class State {
    kUninitialized,
    kRunning,
    kSuspended,
    kFinished,
  };

 public:
  PosixCondition()
      : thread_(0),
        signaled_(false),
        exit_code_(0),
        state_(State::kUninitialized),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
#endif
  }
  bool Initialize(Thread::CreationParameters params, ThreadStartData* start_data) {
    start_data->create_suspended = params.create_suspended;
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0)
      return false;
    // Package 39: never give a host thread less than 4 MiB of native stack. Recompiled guest
    // code keeps its own stack in guest memory, but the native frames of nested sub_X calls
    // grow with inlining (ThinLTO + direct calls); a guest-sized host stack (as low as
    // 16 KiB) would overflow there. Pages are committed lazily, so this costs address space.
    size_t host_stack_size = params.stack_size;
    if (host_stack_size < (size_t{4} << 20)) host_stack_size = size_t{4} << 20;
    if (pthread_attr_setstacksize(&attr, host_stack_size) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    if (params.initial_priority != 0) {
      sched_param sched{};
      sched.sched_priority = params.initial_priority + 1;
      if (pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
      if (pthread_attr_setschedparam(&attr, &sched) != 0) {
        pthread_attr_destroy(&attr);
        return false;
      }
    }
    if (pthread_create(&thread_, &attr, ThreadStartRoutine, start_data) != 0) {
      pthread_attr_destroy(&attr);
      return false;
    }
    pthread_attr_destroy(&attr);
    return true;
  }

  /// Constructor for existing thread. This should only happen once called by
  /// Thread::GetCurrentThread() on the main thread
  explicit PosixCondition(pthread_t thread)
      : thread_(thread),
        signaled_(false),
        exit_code_(0),
        state_(State::kRunning),
        suspend_count_(0) {
    sem_init(&suspend_sem_, 0, 0);
#if REX_PLATFORM_ANDROID
    android_pre_api_26_name_[0] = '\0';
    // Built by Thread::GetCurrentThread() on the thread itself.
    tid_.store(pid_t(syscall(SYS_gettid)), std::memory_order_release);
#endif
  }

  virtual ~PosixCondition() {
    // Match Canary/Edge behavior.
    // Force-cancel/join from the condition destructor can self-join/crash
    // depending on shutdown ordering, so threads must be stopped explicitly.
  }

  bool Signal() override { return true; }
  bool SignalLocked() override { return true; }

  // Set while user callbacks (APCs) are queued; alertable waits poll it.
  const std::atomic<bool>* pending_user_callbacks_flag() const {
    return &has_pending_user_callbacks_;
  }

  std::string name() const {
    WaitStarted();
    auto result = std::array<char, 17>{'\0'};
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
#if REX_PLATFORM_ANDROID
      // pthread_getname_np was added in API 26 - below that, store the name in
      // this object, which may be only modified through Xenia threading, but
      // should be enough in most cases.
      if (android_pthread_getname_np_) {
        if (android_pthread_getname_np_(thread_, result.data(), result.size() - 1) != 0) {
          assert_always();
        }
      } else {
        std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
        rex::string::copy_truncating(result.data(), android_pre_api_26_name_, result.size());
      }
#else
      if (pthread_getname_np(thread_, result.data(), result.size() - 1) != 0) {
        assert_always();
      }
#endif
    }
    return std::string(result.data());
  }

  void set_name(const std::string& name) {
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (state_ != State::kUninitialized && state_ != State::kFinished) {
      pthread_setname_np(thread_, std::string(name).c_str());
#if REX_PLATFORM_ANDROID
      SetAndroidPreApi26Name(name);
#endif
    }
  }

#if REX_PLATFORM_ANDROID
  void SetAndroidPreApi26Name(const std::string_view name) {
    if (android_pthread_getname_np_) {
      return;
    }
    std::lock_guard<std::mutex> lock(android_pre_api_26_name_mutex_);
    rex::string::copy_truncating(android_pre_api_26_name_, name,
                                 rex::countof(android_pre_api_26_name_));
  }
#endif

  uint32_t system_id() const { return static_cast<uint32_t>(thread_); }
#if REX_PLATFORM_ANDROID
  // Kernel tid saved by the thread itself when it starts. bionic's pthread_gettid_np()
  // aborts the whole process ("invalid pthread_t passed to pthread_gettid_np") when the
  // thread has already exited and been joined, so it is never called on another thread.
  // WaitStarted(): the tid is stored before the state leaves kUninitialized, so it is never 0
  // here (a 0 tid would make setpriority()/sched_setaffinity() act on the calling thread).
  uint32_t native_tid() const {
    WaitStarted();
    return static_cast<uint32_t>(tid_.load(std::memory_order_acquire));
  }
  bool finished() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    return state_ == State::kFinished;
  }
#endif

  uint64_t affinity_mask() {
    WaitStarted();
    cpu_set_t cpu_set;
#if REX_PLATFORM_ANDROID
    CPU_ZERO(&cpu_set);
    const pid_t tid = tid_.load(std::memory_order_acquire);
    if (finished() || tid <= 0 || sched_getaffinity(tid, sizeof(cpu_set_t), &cpu_set) != 0) {
      return 0;
    }
#else
    if (pthread_getaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
    uint64_t result = 0;
    auto cpu_count = std::min(CPU_SETSIZE, 64);
    for (auto i = 0u; i < cpu_count; i++) {
      auto set = CPU_ISSET(i, &cpu_set);
      result |= set << i;
    }
    return result;
  }

  void set_affinity_mask(uint64_t mask) {
    WaitStarted();
    cpu_set_t cpu_set;
    CPU_ZERO(&cpu_set);
    for (auto i = 0u; i < 64; i++) {
      if (mask & (1ULL << i)) {
        CPU_SET(i, &cpu_set);
      }
    }
#if REX_PLATFORM_ANDROID
    const pid_t tid = tid_.load(std::memory_order_acquire);
    if (finished() || tid <= 0) return;
    sched_setaffinity(tid, sizeof(cpu_set_t), &cpu_set);  // may race the thread's exit: ignore
#else
    if (pthread_setaffinity_np(thread_, sizeof(cpu_set_t), &cpu_set) != 0) {
      assert_always();
    }
#endif
  }

  int priority() {
    WaitStarted();
    int policy;
    sched_param param{};
    int ret = pthread_getschedparam(thread_, &policy, &param);
    if (ret != 0) {
      return -1;
    }

    return param.sched_priority;
  }

  void set_priority(int new_priority) {
    WaitStarted();
#if REX_PLATFORM_ANDROID
    // Apps may not use SCHED_FIFO; only creation-flag requests land here.
    __android_log_print(ANDROID_LOG_WARN, "LibertyRecomp", "thread-priority-flag tid=%d value=%d",
                        int(tid_.load(std::memory_order_acquire)), new_priority);
    return;
#endif
    sched_param param{};
    param.sched_priority = new_priority;
    int result = pthread_setschedparam(thread_, SCHED_FIFO, &param);
    if (result != 0) {
      switch (result) {
        case EPERM:
          REXSYS_WARN("set_priority: permission denied");
          break;
        case EINVAL:
          assert_always();
          break;
        default:
          REXSYS_WARN("set_priority: pthread_setschedparam failed ({})", result);
          break;
      }
    }
  }

  void QueueUserCallback(std::function<void()> callback) {
    WaitStarted();
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      user_callbacks_.push_back(std::move(callback));
      has_pending_user_callbacks_.store(true, std::memory_order_release);
    }

    // If the callback is queued on the current thread, don't self-signal.
    // Alertable waits drain this queue in normal thread context.
    if (pthread_equal(thread_, pthread_self())) {
      if (alertable_state_) {
        DispatchQueuedUserCallbacks();
      }
      return;
    }

    sigval value{};
    value.sival_ptr = this;
#if REX_PLATFORM_ANDROID
    // The callback stays queued; a finished thread just never runs it (no signal, no abort).
    const pid_t tid = tid_.load(std::memory_order_acquire);
    if (finished() || tid <= 0) return;
    int result = sigqueue(tid, GetSystemSignal(SignalType::kThreadUserCallback), value);
#else
    int result = pthread_sigqueue(thread_, GetSystemSignal(SignalType::kThreadUserCallback), value);
#endif
    if (result != 0) {
      REXSYS_WARN("QueueUserCallback: signal delivery failed ({})", result);
    }
  }

  bool DispatchQueuedUserCallbacks() {
    if (!has_pending_user_callbacks_.load(std::memory_order_acquire)) {
      return false;
    }

    std::function<void()> callback;
    {
      std::unique_lock<std::mutex> lock(callback_mutex_);
      if (user_callbacks_.empty()) {
        has_pending_user_callbacks_.store(false, std::memory_order_release);
        return false;
      }
      callback = std::move(user_callbacks_.front());
      user_callbacks_.pop_front();
      has_pending_user_callbacks_.store(!user_callbacks_.empty(), std::memory_order_release);
    }
    if (callback) {
      callback();
    }
    return true;
  }

  bool Resume(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    std::unique_lock<std::mutex> lock(state_mutex_);
    if (suspend_count_ == 0) {
      return false;
    }
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = suspend_count_;
    }
    --suspend_count_;
    if (suspend_count_ == 0 && state_ == State::kSuspended) {
      state_ = State::kRunning;
      // Async-signal-safe wakeup for WaitSuspended() from signal handler path.
      sem_post(&suspend_sem_);
    }
    state_signal_.notify_all();
    return true;
  }

  bool Suspend(uint32_t* out_previous_suspend_count = nullptr) {
    if (out_previous_suspend_count) {
      *out_previous_suspend_count = 0;
    }
    WaitStarted();
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (out_previous_suspend_count) {
        *out_previous_suspend_count = suspend_count_;
      }
      state_ = State::kSuspended;
      ++suspend_count_;
    }

    if (is_current_thread) {
      WaitSuspended();
      return true;
    }

    int result = pthread_kill(thread_, GetSystemSignal(SignalType::kThreadSuspend));
    return result == 0;
  }

  void Terminate(int exit_code) {
    bool is_current_thread = pthread_self() == thread_;
    {
      std::unique_lock<std::mutex> lock(state_mutex_);
      if (state_ == State::kFinished) {
        if (is_current_thread) {
          // This is really bad. Some thread must have called Terminate() on us
          // just before we decided to terminate ourselves
          assert_always();
          for (;;) {
            // Wait for pthread_cancel() to actually happen.
          }
        }
        return;
      }
      state_ = State::kFinished;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);

      exit_code_ = exit_code;
      signaled_ = true;
      BumpEpochLocked();  // 35: spinning waiters
      cond_.notify_all();
      NotifyExternalWaiters();
    }
    if (is_current_thread) {
      pthread_exit(reinterpret_cast<void*>(exit_code));
    } else {
#if REX_PLATFORM_ANDROID
      if (pthread_kill(thread_, GetSystemSignal(SignalType::kThreadTerminate)) != 0) {
        assert_always();
      }
#else
      if (pthread_cancel(thread_) != 0) {
        assert_always();
      }
#endif
    }
  }

  void WaitStarted() const {
    std::unique_lock<std::mutex> lock(state_mutex_);
    state_signal_.wait(lock, [this] { return state_ != State::kUninitialized; });
  }

  /// Uses sem_wait because it may be called from signal handler context.
  void WaitSuspended() {
    int ret;
    do {
      ret = sem_wait(&suspend_sem_);
    } while (ret == -1 && errno == EINTR);
  }

  void* native_handle() const override { return reinterpret_cast<void*>(thread_); }

 private:
  static void* ThreadStartRoutine(void* parameter);
  inline bool signaled() const override { return signaled_; }
  inline void post_execution() override {
    if (thread_) {
      pthread_join(thread_, nullptr);
    }
    sem_destroy(&suspend_sem_);
  }
  pthread_t thread_;
#if REX_PLATFORM_ANDROID
  std::atomic<pid_t> tid_{0};
#endif
  bool signaled_;
  int exit_code_;
  State state_;             // Protected by state_mutex_
  uint32_t suspend_count_;  // Protected by state_mutex_
  sem_t suspend_sem_;
  mutable std::mutex state_mutex_;
  mutable std::mutex callback_mutex_;
  mutable std::condition_variable state_signal_;
  std::deque<std::function<void()>> user_callbacks_;
  std::atomic<bool> has_pending_user_callbacks_{false};
#if REX_PLATFORM_ANDROID
  // Name accessible via name() on Android before API 26 which added
  // pthread_getname_np.
  mutable std::mutex android_pre_api_26_name_mutex_;
  char android_pre_api_26_name_[16];
#endif
};

class PosixWaitHandle {
 public:
  virtual ~PosixWaitHandle();
  virtual PosixConditionBase& condition() = 0;
};

PosixWaitHandle::~PosixWaitHandle() = default;

thread_local PosixCondition<Thread>* current_thread_condition_ = nullptr;

bool DispatchCurrentThreadUserCallback() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ && current_thread_condition_->DispatchQueuedUserCallbacks();
}

// The alert flag of the calling thread (pending user callbacks), for alertable waits.
const std::atomic<bool>* CurrentThreadPendingUserCallbacksFlag() {
  if (!current_thread_condition_) {
    Thread::GetCurrentThread();
  }
  return current_thread_condition_ ? current_thread_condition_->pending_user_callbacks_flag()
                                   : nullptr;
}

namespace {

class ScopedAlertableState {
 public:
  explicit ScopedAlertableState(bool alertable) : alertable_(alertable) {
    if (alertable_) {
      alertable_state_ = true;
    }
  }
  ~ScopedAlertableState() {
    if (alertable_) {
      alertable_state_ = false;
    }
  }

 private:
  bool alertable_;
};

}  // namespace

// This wraps a condition object as our handle because posix has no single
// native handle for higher level concurrency constructs such as semaphores
template <typename T>
class PosixConditionHandle : public T, public PosixWaitHandle {
 public:
  PosixConditionHandle() = default;
  explicit PosixConditionHandle(bool);
  explicit PosixConditionHandle(pthread_t thread);
  PosixConditionHandle(bool manual_reset, bool initial_state);
  PosixConditionHandle(uint32_t initial_count, uint32_t maximum_count);
  ~PosixConditionHandle() override = default;

  PosixCondition<T>& condition() override { return handle_; }
  void* native_handle() const override { return handle_.native_handle(); }

 protected:
  PosixCondition<T> handle_;
  friend PosixCondition<T>;
};

template <>
PosixConditionHandle<Semaphore>::PosixConditionHandle(uint32_t initial_count,
                                                      uint32_t maximum_count)
    : handle_(initial_count, maximum_count) {}

template <>
PosixConditionHandle<Mutant>::PosixConditionHandle(bool initial_owner) : handle_(initial_owner) {}

template <>
PosixConditionHandle<Timer>::PosixConditionHandle(bool manual_reset) : handle_(manual_reset) {}

template <>
PosixConditionHandle<Event>::PosixConditionHandle(bool manual_reset, bool initial_state)
    : handle_(manual_reset, initial_state) {}

template <>
PosixConditionHandle<Thread>::PosixConditionHandle(pthread_t thread) : handle_(thread) {}

WaitResult Wait(WaitHandle* wait_handle, bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle = dynamic_cast<PosixWaitHandle*>(wait_handle);
  if (posix_wait_handle == nullptr) {
    return WaitResult::kFailed;
  }
  auto& condition = posix_wait_handle->condition();
  const WaitDeadline deadline(timeout);
  if (!is_alertable) {
    return condition.WaitUntil(nullptr, deadline, nullptr);
  }

  ScopedAlertableState alertable_state_guard(true);
  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return WaitResult::kUserCallback;
    }
    // Stays registered on the object until it is signalled, the deadline passes or a
    // user callback is queued (then the loop dispatches it).
    auto result = condition.WaitUntil(nullptr, deadline, alert_flag);
    if (result != WaitResult::kUserCallback) {
      return result;
    }
  }
}

WaitResult SignalAndWait(WaitHandle* wait_handle_to_signal, WaitHandle* wait_handle_to_wait_on,
                         bool is_alertable, std::chrono::milliseconds timeout) {
  auto posix_wait_handle_to_signal = dynamic_cast<PosixWaitHandle*>(wait_handle_to_signal);
  auto posix_wait_handle_to_wait_on = dynamic_cast<PosixWaitHandle*>(wait_handle_to_wait_on);
  if (posix_wait_handle_to_signal == nullptr || posix_wait_handle_to_wait_on == nullptr) {
    return WaitResult::kFailed;
  }
  auto& to_signal = posix_wait_handle_to_signal->condition();
  auto& to_wait_on = posix_wait_handle_to_wait_on->condition();
  const WaitDeadline deadline(timeout);
  if (!is_alertable) {
    return to_wait_on.WaitUntil(&to_signal, deadline, nullptr);
  }

  ScopedAlertableState alertable_state_guard(true);
  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
  // The object is signalled exactly once, together with the first registration.
  PosixConditionBase* pending_signal = &to_signal;
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      if (pending_signal && !pending_signal->Signal()) {
        return WaitResult::kFailed;
      }
      return WaitResult::kUserCallback;
    }
    auto result = to_wait_on.WaitUntil(pending_signal, deadline, alert_flag);
    pending_signal = nullptr;
    if (result != WaitResult::kUserCallback) {
      return result;
    }
  }
}

std::pair<WaitResult, size_t> WaitMultiple(WaitHandle* wait_handles[], size_t wait_handle_count,
                                           bool wait_all, bool is_alertable,
                                           std::chrono::milliseconds timeout) {
  std::vector<PosixConditionBase*> conditions;
  conditions.reserve(wait_handle_count);
  for (size_t i = 0u; i < wait_handle_count; ++i) {
    auto handle = dynamic_cast<PosixWaitHandle*>(wait_handles[i]);
    if (handle == nullptr) {
      return std::make_pair(WaitResult::kFailed, 0);
    }
    conditions.push_back(&handle->condition());
  }
  const WaitDeadline deadline(timeout);
  if (!is_alertable) {
    return PosixConditionBase::WaitMultipleUntil(conditions, wait_all, deadline, nullptr);
  }

  ScopedAlertableState alertable_state_guard(true);
  const std::atomic<bool>* alert_flag = CurrentThreadPendingUserCallbacksFlag();
  while (true) {
    if (DispatchCurrentThreadUserCallback()) {
      return std::make_pair(WaitResult::kUserCallback, 0);
    }
    auto result = PosixConditionBase::WaitMultipleUntil(conditions, wait_all, deadline, alert_flag);
    if (result.first != WaitResult::kUserCallback) {
      return result;
    }
  }
}

class PosixEvent : public PosixConditionHandle<Event> {
 public:
  PosixEvent(bool manual_reset, bool initial_state)
      : PosixConditionHandle(manual_reset, initial_state) {}
  ~PosixEvent() override = default;
  void Set() override { handle_.Signal(); }
  void Reset() override { handle_.Reset(); }
  // Releases the threads waiting now, atomically (no Set / sleep / Reset window).
  void Pulse() override { handle_.Pulse(); }
};

std::unique_ptr<Event> Event::CreateManualResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(true, initial_state);
}

std::unique_ptr<Event> Event::CreateAutoResetEvent(bool initial_state) {
  return std::make_unique<PosixEvent>(false, initial_state);
}

class PosixSemaphore : public PosixConditionHandle<Semaphore> {
 public:
  PosixSemaphore(int initial_count, int maximum_count)
      : PosixConditionHandle(static_cast<uint32_t>(initial_count),
                             static_cast<uint32_t>(maximum_count)) {}
  ~PosixSemaphore() override = default;
  bool Release(int release_count, int* out_previous_count) override {
    if (release_count < 1) {
      return false;
    }
    return handle_.Release(static_cast<uint32_t>(release_count), out_previous_count);
  }
};

std::unique_ptr<Semaphore> Semaphore::Create(int initial_count, int maximum_count) {
  if (initial_count < 0 || initial_count > maximum_count || maximum_count <= 0) {
    return nullptr;
  }
  return std::make_unique<PosixSemaphore>(initial_count, maximum_count);
}

class PosixMutant : public PosixConditionHandle<Mutant> {
 public:
  explicit PosixMutant(bool initial_owner) : PosixConditionHandle(initial_owner) {}
  ~PosixMutant() override = default;
  bool Release() override { return handle_.Release(); }
};

std::unique_ptr<Mutant> Mutant::Create(bool initial_owner) {
  return std::make_unique<PosixMutant>(initial_owner);
}

class PosixTimer : public PosixConditionHandle<Timer> {
  using WClock_ = Timer::WClock_;
  using GClock_ = Timer::GClock_;

 public:
  explicit PosixTimer(bool manual_reset) : PosixConditionHandle(manual_reset) {}
  ~PosixTimer() override = default;

  bool SetOnceAfter(rex::chrono::hundrednanoseconds rel_time,
                    std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(GClock_::now() + rel_time, std::move(opt_callback));
  }
  bool SetOnceAt(WClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    return SetOnceAt(std::chrono::clock_cast<GClock_>(due_time), std::move(opt_callback));
  };
  bool SetOnceAt(GClock_::time_point due_time,
                 std::function<void()> opt_callback = nullptr) override {
    handle_.SetOnce(due_time, std::move(opt_callback));
    return true;
  }

  bool SetRepeatingAfter(rex::chrono::hundrednanoseconds rel_time, std::chrono::milliseconds period,
                         std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(GClock_::now() + rel_time, period, std::move(opt_callback));
  }
  bool SetRepeatingAt(WClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    return SetRepeatingAt(std::chrono::clock_cast<GClock_>(due_time), period,
                          std::move(opt_callback));
  }
  bool SetRepeatingAt(GClock_::time_point due_time, std::chrono::milliseconds period,
                      std::function<void()> opt_callback = nullptr) override {
    handle_.SetRepeating(due_time, period, std::move(opt_callback));
    return true;
  }
  bool Cancel() override {
    handle_.Cancel();
    return true;
  }
};

std::unique_ptr<Timer> Timer::CreateManualResetTimer() {
  return std::make_unique<PosixTimer>(true);
}

std::unique_ptr<Timer> Timer::CreateSynchronizationTimer() {
  return std::make_unique<PosixTimer>(false);
}

class PosixThread : public PosixConditionHandle<Thread> {
 public:
  PosixThread() = default;
  explicit PosixThread(pthread_t thread) : PosixConditionHandle(thread) {}
  ~PosixThread() override = default;

  bool Initialize(CreationParameters params, std::function<void()> start_routine) {
    auto start_data = new ThreadStartData({std::move(start_routine), false, this});
    return handle_.Initialize(params, start_data);
  }

  void set_name(std::string name) override {
    handle_.WaitStarted();
    Thread::set_name(name);
    if (name.length() > 15) {
      name = name.substr(0, 15);
    }
    handle_.set_name(name);
  }

  uint32_t system_id() const override { return handle_.system_id(); }
#if REX_PLATFORM_ANDROID
  uint32_t system_id_native() const override { return handle_.native_tid(); }
#endif

  uint64_t affinity_mask() override { return handle_.affinity_mask(); }
  void set_affinity_mask(uint64_t mask) override { handle_.set_affinity_mask(mask); }

  int priority() override { return handle_.priority(); }
  void set_priority(int new_priority) override { handle_.set_priority(new_priority); }

  void QueueUserCallback(std::function<void()> callback) override {
    handle_.QueueUserCallback(std::move(callback));
  }

  bool Resume(uint32_t* out_previous_suspend_count) override {
    return handle_.Resume(out_previous_suspend_count);
  }

  bool Suspend(uint32_t* out_previous_suspend_count) override {
    return handle_.Suspend(out_previous_suspend_count);
  }

  void Terminate(int exit_code) override { handle_.Terminate(exit_code); }

  void WaitSuspended() { handle_.WaitSuspended(); }
};

thread_local PosixThread* current_thread_ = nullptr;

void* PosixCondition<Thread>::ThreadStartRoutine(void* parameter) {
#if !REX_PLATFORM_ANDROID
  if (pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, nullptr) != 0) {
    assert_always();
  }
#endif
  set_current_thread_name("");

  auto start_data = static_cast<ThreadStartData*>(parameter);
  assert_not_null(start_data);
  assert_not_null(start_data->thread_obj);

  auto thread = dynamic_cast<PosixThread*>(start_data->thread_obj);
  auto start_routine = std::move(start_data->start_routine);
  auto create_suspended = start_data->create_suspended;
  delete start_data;

  current_thread_ = thread;
  current_thread_condition_ = &thread->handle_;
#if REX_PLATFORM_ANDROID
  thread->handle_.tid_.store(pid_t(syscall(SYS_gettid)), std::memory_order_release);
  // Linux rounds every timed sleep of a normal thread up by up to 50 us ("timer slack"),
  // so short waits (guest Sleep, alertable polling) oversleep 2-6x. 1 us keeps them short.
  prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
#endif
  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.suspend_count_ = create_suspended ? 1 : 0;
    thread->handle_.state_ = create_suspended ? State::kSuspended : State::kRunning;
    thread->handle_.state_signal_.notify_all();
    if (create_suspended) {
      thread->handle_.state_signal_.wait(lock,
                                         [thread] { return thread->handle_.suspend_count_ == 0; });
    }
  }

  start_routine();

  {
    std::unique_lock<std::mutex> lock(thread->handle_.state_mutex_);
    thread->handle_.state_ = State::kFinished;
  }

  {
    std::unique_lock<std::mutex> lock(thread->handle_.mutex_);
    thread->handle_.exit_code_ = 0;
    thread->handle_.signaled_ = true;
    thread->handle_.BumpEpochLocked();  // 35: spinning waiters
    thread->handle_.cond_.notify_all();
    thread->handle_.NotifyExternalWaiters();
  }

  current_thread_ = nullptr;
  current_thread_condition_ = nullptr;
  return nullptr;
}

std::unique_ptr<Thread> Thread::Create(CreationParameters params,
                                       std::function<void()> start_routine) {
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif
  auto thread = std::make_unique<PosixThread>();
  if (!thread->Initialize(params, std::move(start_routine)))
    return nullptr;
  assert_not_null(thread);
  return thread;
}

Thread* Thread::GetCurrentThread() {
  if (current_thread_) {
    return current_thread_;
  }

  // Threads not created by Thread::Create (typically main thread) still need
  // process-wide signal handlers used by suspend/APC machinery.
  install_signal_handler(SignalType::kThreadSuspend);
  install_signal_handler(SignalType::kThreadUserCallback);
#if REX_PLATFORM_ANDROID
  install_signal_handler(SignalType::kThreadTerminate);
#endif

  // Should take this route only for threads not created by Thread::Create.
  // The only thread not created by Thread::Create should be the main thread.
  pthread_t handle = pthread_self();

  current_thread_ = new PosixThread(handle);
  current_thread_condition_ = &current_thread_->condition();
  // TODO(bwrsandman): Disabling deleting thread_local current thread to prevent
  //                   assert in destructor. Since this is thread local, the
  //                   "memory leaking" is controlled.
  // atexit([] { delete current_thread_; });

  return current_thread_;
}

void Thread::Exit(int exit_code) {
  if (current_thread_) {
    current_thread_->Terminate(exit_code);
  } else {
    // Should only happen with the main thread
    pthread_exit(reinterpret_cast<void*>(exit_code));
  }
  // Function must not return
  assert_always();
}

void set_current_thread_name(const std::string_view name) {
  pthread_setname_np(pthread_self(), std::string(name).c_str());
#if REX_PLATFORM_ANDROID
  if (!android_pthread_getname_np_ && current_thread_) {
    current_thread_->condition().SetAndroidPreApi26Name(name);
  }
#endif
}

static void signal_handler(int signal, siginfo_t* /*info*/, void* /*context*/) {
  switch (GetSystemSignalType(signal)) {
    case SignalType::kThreadSuspend: {
      if (!current_thread_) {
        return;
      }
      current_thread_->WaitSuspended();
    } break;
    case SignalType::kThreadUserCallback: {
      // Callbacks are drained from alertable waits in normal thread context.
      // This signal is only used as a wakeup hint.
    } break;
#if REX_PLATFORM_ANDROID
    case SignalType::kThreadTerminate: {
      pthread_exit(reinterpret_cast<void*>(-1));
    } break;
#endif
    default:
      assert_always();
  }
}

}  // namespace rex::thread
// 53: per-thread wait spin budget (see t_wait_spin_ns). Called by the thread itself, e.g. by
// the RAGE job / physics worker entry hooks (gta4-recomp android_job_probe.cpp).
//   spin_us: budget of one wait in microseconds (capped at 2000); UINT32_MAX restores the
//            global budget (REX_WAIT_SPIN_US, 30 us).
//   flags:   bit 0 = wait in WFE while spinning (only with the kernel's event stream);
//            bit 1 = only while running on a lowest-capacity CPU (little cluster).
extern "C" __attribute__((visibility("default"))) void rex_thread_set_wait_spin(uint32_t spin_us,
                                                                                uint32_t flags) {
  if (spin_us == UINT32_MAX) {
    rex::thread::t_wait_spin_ns = UINT32_MAX;
    rex::thread::t_wait_spin_wfe = false;
    rex::thread::t_wait_spin_little_only = false;
    return;
  }
  rex::thread::t_wait_spin_ns = std::min<uint32_t>(spin_us, 2000u) * 1000u;
  rex::thread::t_wait_spin_wfe = (flags & 1u) != 0;
  rex::thread::t_wait_spin_little_only = (flags & 2u) != 0;
#if REX_PLATFORM_ANDROID
  // Once per distinct setting (job and physics workers): what the threads got (the WFE mode
  // needs the event stream, the little-only mode the capacity table).
  static std::mutex log_mutex;
  static uint64_t logged[4] = {};
  const uint64_t key = (uint64_t(spin_us) << 32) | flags | (1ull << 31);
  bool fresh = false;
  {
    std::lock_guard<std::mutex> lock(log_mutex);
    for (uint64_t& slot : logged) {
      if (slot == key) break;
      if (!slot) {
        slot = key;
        fresh = true;
        break;
      }
    }
  }
  if (fresh) {
#if defined(__aarch64__)
    const bool stream = rex::thread::WfeEventStream();
#else
    const bool stream = false;
#endif
    __android_log_print(ANDROID_LOG_WARN, "LibertyThreads",
                        "wait-spin: worker budget %u us, wfe requested %d, event stream %d, "
                        "little only %d (little cpus 0x%llX)",
                        spin_us, (flags & 1u) ? 1 : 0, stream ? 1 : 0, (flags & 2u) ? 1 : 0,
                        static_cast<unsigned long long>(rex::thread::LowCapacityCpuMask()));
  }
#endif
}
