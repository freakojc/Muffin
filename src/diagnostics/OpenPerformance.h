#pragma once

#include <QLoggingCategory>
#include <chrono>

#include "diagnostics/ProcessMemory.h"

namespace muffin::diag {
inline const QLoggingCategory& openPerformanceCategory() {
  static const QLoggingCategory category("muffin.perf.open", QtWarningMsg);
  return category;
}
// Independent of the per-inline parser diagnostics: only a few coarse scopes
// are sampled per open. Disabled scopes neither read the clock nor query memory.
class OpenPerfAccumulator {
public:
  explicit OpenPerfAccumulator(const char* name) : name_(name), enabled_(openPerformanceCategory().isDebugEnabled()) {}
  ~OpenPerfAccumulator() {
    if (enabled_)
      qCDebug(openPerformanceCategory).nospace() << name_ << " " << elapsedNs_ / 1e6 << " ms " << workingSetBytes();
  }
  template <typename F> void measure(F&& operation) {
    if (!enabled_) {
      operation();
      return;
    }
    const auto start = std::chrono::steady_clock::now();
    operation();
    elapsedNs_ +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
  }

private:
  const char* name_;
  bool enabled_;
  qint64 elapsedNs_ = 0;
};
class OpenPerfScope {
public:
  explicit OpenPerfScope(const char* name) : name_(name), enabled_(openPerformanceCategory().isDebugEnabled()) {
    if (enabled_)
      start_ = std::chrono::steady_clock::now();
  }
  ~OpenPerfScope() {
    if (enabled_) {
      const auto ns =
          std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_).count();
      qCDebug(openPerformanceCategory).nospace() << name_ << " " << ns / 1e6 << " ms " << workingSetBytes();
    }
  }

private:
  const char* name_;
  bool enabled_;
  std::chrono::steady_clock::time_point start_;
};
} // namespace muffin::diag
