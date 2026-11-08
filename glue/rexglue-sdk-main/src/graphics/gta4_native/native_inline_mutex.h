// Package 41: a small futex mutex whose uncontended lock/unlock are inline LSE atomics
// (casa / swpl with -march=armv8.2-a+lse), for command_capture_mutex_.
//
// Why: on the 888 trace of 30.10 20:16 RenderGuest spends 7.3 % of its CPU in
// pthread_mutex_lock/unlock of command_capture_mutex_ (bionic: out-of-line call, type
// checks, then libc's own outline-atomic helper __aarch64_swp2_rel / __aarch64_cas2_acq).
// The mutex is taken once per title command (SubmitTitleCommand), almost always by the
// same thread and uncontended. Main XThread waits for RenderGuest ~2.5 ms per frame.
//
// Algorithm: Drepper, "Futexes Are Tricky", mutex 2 (0 free, 1 locked, 2 locked with
// waiters). Satisfies BasicLockable/Lockable, so std::unique_lock / std::lock_guard work
// unchanged. Not recursive, not usable with std::condition_variable (the capture mutex
// is used with neither).
//
// Package 53: store-release unlock (SetStoreReleaseUnlock, chosen once before first use).
// 888, batch 52 (M1_control), by instruction address: the unlock's swpl costs RenderGuest
// 1.14 ms of its 27.5 ms frame, the lock's casa 0.32 ms. The time is not the atomic itself:
// swpl is a release RMW whose result the next instruction needs, so the pipeline waits until
// every store of the capture (the command slot, payload buffers: lines last written on the
// backend's and the retirer's cores) has drained. A plain store-release (stlr) orders the same
// stores but returns nothing, so RenderGuest runs on into guest code while they drain.
// The price: stlr cannot see waiters atomically. Waiters therefore count themselves in
// waiters_ and sleep with a 100 us timeout; the unlocker wakes them when it sees the count,
// and a wake missed in the stlr/load window is caught by the timeout. Contention is rare:
// in the 888 traces only RenderGuest takes this mutex while playing (Main XThread once in 20 s,
// for a texture lock).
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>

#if defined(__linux__) || defined(__ANDROID__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace gta4 {

class InlineMutex {
 public:
  InlineMutex() = default;
  InlineMutex(const InlineMutex&) = delete;
  InlineMutex& operator=(const InlineMutex&) = delete;

  // 53: before the first lock, while no other thread can use the mutex.
  void SetStoreReleaseUnlock(bool on) noexcept { store_release_ = on; }
  bool store_release_unlock() const noexcept { return store_release_; }

  void lock() noexcept {
    uint32_t expected = 0;
    if (state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                       std::memory_order_relaxed)) [[likely]] {
      return;
    }
    if (store_release_) {
      LockSlowPolling();
      return;
    }
    LockSlow(expected);
  }

  bool try_lock() noexcept {
    uint32_t expected = 0;
    return state_.compare_exchange_strong(expected, 1, std::memory_order_acquire,
                                          std::memory_order_relaxed);
  }

  void unlock() noexcept {
    if (store_release_) {
      // 53: stlr; the waiters_ load may be satisfied before the store is visible (that is
      // the window the waiters' timeout covers).
      state_.store(0, std::memory_order_release);
      if (waiters_.load(std::memory_order_relaxed)) [[unlikely]] Wake();
      return;
    }
    if (state_.exchange(0, std::memory_order_release) != 1) [[unlikely]] {
      Wake();
    }
  }

 private:
  // 53: store-release mode. The state stays 0 / 1 (no "locked with waiters" value: the
  // unlock does not read it).
  [[gnu::noinline]] void LockSlowPolling() noexcept {
    for (int i = 0; i < 100; ++i) {
      Relax();
      if (state_.load(std::memory_order_relaxed) == 0 && try_lock()) return;
    }
    waiters_.fetch_add(1, std::memory_order_relaxed);
    while (!try_lock()) WaitTimed();
    waiters_.fetch_sub(1, std::memory_order_relaxed);
  }

  void WaitTimed() noexcept {
#if defined(__linux__) || defined(__ANDROID__)
    const timespec timeout{0, 100'000};  // 100 us
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&state_), FUTEX_WAIT_PRIVATE, 1u, &timeout,
            nullptr, 0);
#else
    Relax();
#endif
  }

  [[gnu::noinline]] void LockSlow(uint32_t seen) noexcept {
    // Short spin first: the holder's critical section is a command copy (microseconds).
    for (int i = 0; i < 100; ++i) {
      if (seen == 0) {
        uint32_t expected = 0;
        if (state_.compare_exchange_weak(expected, 1, std::memory_order_acquire,
                                         std::memory_order_relaxed)) {
          return;
        }
        seen = expected;
      } else {
        Relax();
        seen = state_.load(std::memory_order_relaxed);
      }
    }
    if (seen != 2) seen = state_.exchange(2, std::memory_order_acquire);
    while (seen != 0) {
      Wait();
      seen = state_.exchange(2, std::memory_order_acquire);
    }
  }

  static void Relax() noexcept {
#if defined(__aarch64__)
    asm volatile("yield" ::: "memory");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
  }

  void Wait() noexcept {
#if defined(__linux__) || defined(__ANDROID__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&state_), FUTEX_WAIT_PRIVATE, 2u, nullptr,
            nullptr, 0);
#endif
  }

  [[gnu::noinline]] void Wake() noexcept {
#if defined(__linux__) || defined(__ANDROID__)
    syscall(SYS_futex, reinterpret_cast<uint32_t*>(&state_), FUTEX_WAKE_PRIVATE, 1, nullptr,
            nullptr, 0);
#endif
  }

  static_assert(sizeof(std::atomic<uint32_t>) == sizeof(uint32_t));
  std::atomic<uint32_t> state_{0};
  std::atomic<uint32_t> waiters_{0};  // 53: store-release mode only
  bool store_release_ = false;        // 53: set once before first use
};

}  // namespace gta4
