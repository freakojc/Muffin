#pragma once

#include <QLoggingCategory>
#include <QWidget>

namespace muffin::diag {

// Opt-in timestamp at the actual dirty request, before Qt coalesces updates.
// Keep normal QWidget::update semantics; never force a synchronous repaint.
inline void updateViewport(QWidget* viewport, const QRect& rect) {
  static const QLoggingCategory category("muffin.perf.queue", QtWarningMsg);
  if (category.isDebugEnabled() && viewport->isVisible() && viewport->updatesEnabled() &&
      rect.intersects(viewport->rect()))
    qCDebug(category).nospace() << "viewport.update " << quintptr(viewport) << ' ' << rect.x() << ' ' << rect.y() << ' '
                                << rect.width() << ' ' << rect.height();
  viewport->update(rect);
}

inline void updateViewport(QWidget* viewport) {
  updateViewport(viewport, viewport->rect());
}

} // namespace muffin::diag
