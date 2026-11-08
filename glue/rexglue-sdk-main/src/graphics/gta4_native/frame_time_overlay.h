#pragma once

#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstring>

#ifndef REX_ENABLE_FRAME_TIME_OVERLAY
#define REX_ENABLE_FRAME_TIME_OVERLAY 1
#endif

#if REX_ENABLE_FRAME_TIME_OVERLAY

#if defined(__GNUC__) || defined(__clang__)
#define FT_EXPORT __attribute__((visibility("default")))
#else
#define FT_EXPORT
#endif

class FT_EXPORT FrameTimeOverlay {
public:
  static constexpr size_t kHistorySize = 240;

  void OnFrameEnd();

  float GetAverageFps() const;
  float GetAverageFrameTimeMs() const;
  float GetOnePercentLowMs() const;
  float GetPointOnePercentLowMs() const;
  float GetMaxFrameTimeMs() const;

  const float* history() const { return history_; }
  size_t history_count() const { return history_count_; }
  size_t history_pos() const { return history_pos_; }

  void SetEnabled(bool v) { enabled_ = v; }
  bool IsEnabled() const { return enabled_; }

private:
  uint64_t last_ns_ = 0;
  float history_[kHistorySize]{};
  size_t history_pos_ = 0;
  size_t history_count_ = 0;
  bool enabled_ = true;

  static uint64_t NowNs();
};

// Объявление (не inline-определение!)
FT_EXPORT extern FrameTimeOverlay g_frame_time_overlay;

#endif