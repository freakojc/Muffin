#include "diagnostics/ThemePerformance.h"

namespace muffin {
namespace {
thread_local bool enabled = false;
thread_local ThemePerfSamples samples;
thread_local ThemeTextPaintStats textPaint;
thread_local ThemePerfScope* current = nullptr;
} // namespace
void enableThemePerformance(bool value) {
  enabled = value;
}
void resetThemePerformance() {
  Q_ASSERT(!current);
  samples = {};
  textPaint = {};
}
ThemePerfSamples themePerformanceSamples() {
  return samples;
}
ThemeTextPaintStats themeTextPaintStats() {
  return textPaint;
}
void recordThemeTextPaint(qsizetype lines, qsizetype formats, qsizetype selections) {
  if (!enabled)
    return;
  ++textPaint.calls;
  textPaint.lines += quint64(lines);
  textPaint.formatRanges += quint64(formats);
  textPaint.selectionRanges += quint64(selections);
}
void countThemePerformanceWork(ThemePerfStage stage, quint64 count) {
  if (enabled)
    samples[size_t(stage)].work += count;
}
ThemePerfScope::ThemePerfScope(ThemePerfStage stage) : stage_(stage), active_(enabled) {
  if (!active_)
    return;
  parent_ = current;
  current = this;
  start_ = std::chrono::steady_clock::now();
  ++samples[size_t(stage)].calls;
}
ThemePerfScope::~ThemePerfScope() {
  if (!active_)
    return;
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();
  samples[size_t(stage_)].exclusiveNs += elapsed - childNs_;
  if (parent_)
    parent_->childNs_ += elapsed;
  current = parent_;
}
} // namespace muffin
