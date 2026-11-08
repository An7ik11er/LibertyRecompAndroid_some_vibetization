#include "frame_time_overlay.h"

#if REX_ENABLE_FRAME_TIME_OVERLAY

#include <time.h>

uint64_t FrameTimeOverlay::NowNs() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}

void FrameTimeOverlay::OnFrameEnd() {
  const uint64_t now = NowNs();
  if (last_ns_ != 0) {
    const float ft_ms = float(now - last_ns_) / 1e6f;
    const float clamped = std::min(ft_ms, 250.0f);
    history_[history_pos_] = clamped;
    history_pos_ = (history_pos_ + 1) % kHistorySize;
    if (history_count_ < kHistorySize) ++history_count_;
  }
  last_ns_ = now;
}

float FrameTimeOverlay::GetAverageFrameTimeMs() const {
  if (history_count_ == 0) return 0.0f;
  float sum = 0.0f;
  for (size_t i = 0; i < history_count_; ++i) sum += history_[i];
  return sum / float(history_count_);
}

float FrameTimeOverlay::GetAverageFps() const {
  const float avg = GetAverageFrameTimeMs();
  return avg > 0.01f ? 1000.0f / avg : 0.0f;
}

float FrameTimeOverlay::GetMaxFrameTimeMs() const {
  float mx = 0.0f;
  for (size_t i = 0; i < history_count_; ++i) mx = std::max(mx, history_[i]);
  return mx;
}

float FrameTimeOverlay::GetOnePercentLowMs() const {
  if (history_count_ < 10) return GetMaxFrameTimeMs();
  float sorted[kHistorySize];
  std::memcpy(sorted, history_, history_count_ * sizeof(float));
  std::sort(sorted, sorted + history_count_);
  const size_t idx = size_t(float(history_count_) * 0.99f);
  return sorted[std::min(idx, history_count_ - 1)];
}

float FrameTimeOverlay::GetPointOnePercentLowMs() const {
  if (history_count_ < 20) return GetMaxFrameTimeMs();
  float sorted[kHistorySize];
  std::memcpy(sorted, history_, history_count_ * sizeof(float));
  std::sort(sorted, sorted + history_count_);
  const size_t idx = size_t(float(history_count_) * 0.999f);
  return sorted[std::min(idx, history_count_ - 1)];
}

#endif
#if REX_ENABLE_FRAME_TIME_OVERLAY
FrameTimeOverlay g_frame_time_overlay;
#endif