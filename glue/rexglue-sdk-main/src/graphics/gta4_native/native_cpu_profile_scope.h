#pragma once
#include <atomic>
#include <rex/chrono/clock.h>
#include <type_traits>
#include <utility>
#include "native_cpu_profile.h"

namespace rex::graphics::gta4_native::profile {
inline thread_local CpuRecorder* current_cpu_recorder = nullptr;
inline std::atomic<uint32_t> active_cpu_recorder_bindings{0};
inline uint64_t CpuTick() {
  return rex::chrono::Clock::QueryHostTickCount();
}
class CpuScope {
 public:
  explicit CpuScope(CpuOp op) {
    if (active_cpu_recorder_bindings.load(std::memory_order_relaxed) != 0) [[unlikely]] {
      BeginSlow(op);
    }
  }
  ~CpuScope() {
    if (owner_) [[unlikely]] {
      EndSlow();
    }
  }
  void End() {
    if (token_) [[unlikely]] {
      EndSlow();
    }
  }
  CpuScope(const CpuScope&) = delete;
  CpuScope& operator=(const CpuScope&) = delete;

 private:
  [[gnu::noinline]] void BeginSlow(CpuOp op) {
    CpuRecorder* const recorder = current_cpu_recorder;
    if (recorder && recorder->active()) {
      owner_ = recorder;
      token_ = recorder->Enter(op, CpuTick());
    }
  }
  [[gnu::noinline]] void EndSlow() {
    if (token_) {
      owner_->Leave(token_, CpuTick());
      token_ = {};
    }
    owner_ = nullptr;
  }

  CpuRecorder* owner_ = nullptr;
  CpuRecorder::Token token_{};
};
class CpuPhaseScope {
 public:
  explicit CpuPhaseScope(CpuPhase phase) {
    if (active_cpu_recorder_bindings.load(std::memory_order_relaxed) != 0) [[unlikely]] {
      BeginSlow(phase);
    }
  }
  ~CpuPhaseScope() {
    if (owner_) [[unlikely]] {
      EndSlow();
    }
  }
  void Set(CpuPhase phase) {
    if (owner_) [[unlikely]] {
      SetSlow(phase);
    }
  }

 private:
  [[gnu::noinline]] void BeginSlow(CpuPhase phase) {
    owner_ = current_cpu_recorder;
    if (owner_) {
      prior_ = owner_->SetPhase(phase);
    }
  }
  [[gnu::noinline]] void EndSlow() {
    owner_->SetPhase(prior_);
    owner_ = nullptr;
  }
  [[gnu::noinline]] void SetSlow(CpuPhase phase) { owner_->SetPhase(phase); }

  CpuRecorder* owner_ = nullptr;
  CpuPhase prior_ = CpuPhase::kPublish;
};
class CpuContextScope {
 public:
  explicit CpuContextScope(CpuContext context) {
    if (active_cpu_recorder_bindings.load(std::memory_order_relaxed) != 0) [[unlikely]] {
      BeginSlow(context);
    }
  }
  // Builds the context only while a capture is running: per-command contexts
  // otherwise chase pipeline and shader pointers for nothing.
  template <typename MakeContext,
            typename = std::enable_if_t<std::is_invocable_r_v<CpuContext, MakeContext&>>>
  explicit CpuContextScope(MakeContext&& make) {
    if (active_cpu_recorder_bindings.load(std::memory_order_relaxed) != 0) [[unlikely]] {
      BeginLazySlow(std::forward<MakeContext>(make));
    }
  }
  ~CpuContextScope() {
    if (owner_) [[unlikely]] {
      EndSlow();
    }
  }

 private:
  [[gnu::noinline]] void BeginSlow(CpuContext context) {
    owner_ = current_cpu_recorder;
    if (owner_) {
      prior_ = owner_->SetContext(context);
    }
  }
  template <typename MakeContext>
  [[gnu::noinline]] void BeginLazySlow(MakeContext&& make) {
    CpuRecorder* const recorder = current_cpu_recorder;
    if (recorder && recorder->active()) {
      owner_ = recorder;
      prior_ = owner_->SetContext(make());
    }
  }
  [[gnu::noinline]] void EndSlow() {
    owner_->SetContext(prior_);
    owner_ = nullptr;
  }

  CpuRecorder* owner_ = nullptr;
  CpuContext prior_{};
};
class CpuRecorderBinding {
 public:
  explicit CpuRecorderBinding(CpuRecorder* recorder) {
    if (recorder || active_cpu_recorder_bindings.load(std::memory_order_relaxed) != 0) {
      BindSlow(recorder);
    }
  }
  ~CpuRecorderBinding() {
    if (had_binding_) {
      UnbindSlow();
    }
  }

 private:
  [[gnu::noinline]] void BindSlow(CpuRecorder* recorder) {
    had_binding_ = true;
    if (recorder) {
      counted_ = true;
      active_cpu_recorder_bindings.fetch_add(1, std::memory_order_relaxed);
    }
    prior_ = current_cpu_recorder;
    current_cpu_recorder = recorder;
  }
  [[gnu::noinline]] void UnbindSlow() {
    current_cpu_recorder = prior_;
    if (counted_) {
      active_cpu_recorder_bindings.fetch_sub(1, std::memory_order_relaxed);
    }
  }

  CpuRecorder* prior_ = nullptr;
  bool had_binding_ = false;
  bool counted_ = false;
};
template <typename Fn>
decltype(auto) CpuCall(CpuOp op, Fn&& fn) {
  CpuScope scope(op);
  return std::forward<Fn>(fn)();
}
inline uint64_t CalibrateCpuClock() {
  uint64_t best = UINT64_MAX;
  for (unsigned i = 0; i < 256; ++i) {
    auto a = CpuTick();
    auto b = CpuTick();
    if (b >= a)
      best = std::min(best, b - a);
  }
  return best == UINT64_MAX ? 0 : best;
}
}  // namespace rex::graphics::gta4_native::profile
