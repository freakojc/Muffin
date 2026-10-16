#pragma once

#include <QtGlobal>
#include <array>
#include <chrono>

#include "Export.h"

namespace muffin {
enum class ThemePerfStage { Css, Counters, Layout, Paint, Background, Shadow, Text, Count };
struct ThemePerfSample {
  quint64 calls = 0, work = 0;
  qint64 exclusiveNs = 0;
};
using ThemePerfSamples = std::array<ThemePerfSample, size_t(ThemePerfStage::Count)>;
// Opt-in, thread-local measurements. Nested stages subtract child time so CSS
// computation isn't also reported as counter traversal or layout work.
MUFFIN_CORE_EXPORT void enableThemePerformance(bool enabled);
MUFFIN_CORE_EXPORT void resetThemePerformance();
MUFFIN_CORE_EXPORT ThemePerfSamples themePerformanceSamples();
MUFFIN_CORE_EXPORT void countThemePerformanceWork(ThemePerfStage stage, quint64 count = 1);
// QTextLayout paint inputs, rather than inferred glyph/raster work. Dynamic
// selections can make Qt draw every line once per foreground override.
struct ThemeTextPaintStats {
  quint64 calls = 0, lines = 0, formatRanges = 0, selectionRanges = 0;
};
MUFFIN_CORE_EXPORT void recordThemeTextPaint(qsizetype lines, qsizetype formats, qsizetype selections);
MUFFIN_CORE_EXPORT ThemeTextPaintStats themeTextPaintStats();
class MUFFIN_CORE_EXPORT ThemePerfScope {
 public:
  explicit ThemePerfScope(ThemePerfStage stage);
  ~ThemePerfScope();
  ThemePerfScope(const ThemePerfScope&) = delete;
  ThemePerfScope& operator=(const ThemePerfScope&) = delete;

 private:
  ThemePerfStage stage_;
  bool active_;
  ThemePerfScope* parent_ = nullptr;
  std::chrono::steady_clock::time_point start_;
  qint64 childNs_ = 0;
};
}  // namespace muffin
