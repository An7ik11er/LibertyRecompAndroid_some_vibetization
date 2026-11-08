#pragma once
// Метки для Perfetto: TRACE_SCOPE("имя") пишет в трейс отрезок «от этой строки до конца блока { }».
// Когда трейс не снимается, цена метки почти нулевая (одна проверка флага).
// Во время записи каждая метка = системный вызов, поэтому ставь их только в местах,
// которые срабатывают несколько раз за кадр, а не на каждый draw.
#if defined(__ANDROID__)
#include <android/trace.h>
class TraceScope {
 public:
  explicit TraceScope(const char* name) {
    if (ATrace_isEnabled()) {
      ATrace_beginSection(name);
      active_ = true;
    }
  }
  ~TraceScope() {
    if (active_) ATrace_endSection();
  }
  TraceScope(const TraceScope&) = delete;
  TraceScope& operator=(const TraceScope&) = delete;

 private:
  bool active_ = false;
};
#else
class TraceScope {
 public:
  explicit TraceScope(const char*) {}
};
#endif
#define TRACE_SCOPE_CAT2(a, b) a##b
#define TRACE_SCOPE_CAT(a, b) TRACE_SCOPE_CAT2(a, b)
#define TRACE_SCOPE(name) TraceScope TRACE_SCOPE_CAT(trace_scope_, __LINE__)(name)
