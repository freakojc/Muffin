#pragma once

#include <memory>

#include "render/TextLayout.h"
#include "theme/ThemeDefinition.h"

namespace muffin {

struct TextShadowRasterStats {
  quint64 requests = 0, hits = 0, misses = 0, culled = 0, rasterizedPixels = 0, largestSurfacePixels = 0;
  qsizetype cacheBytes = 0, cacheEntries = 0;
};
TextShadowRasterStats textShadowRasterStats();
void resetTextShadowRasterStats();

// One owner per immutable shaped layout. Rebuilding/destroying the layout
// releases its entries from a thread-local, byte-bounded LRU shared by views.
class TextShadowPainter {
 public:
  void reset() { owner_.reset(); }
  void paint(QPainter& painter, const TextLayout& text, QPointF origin, const TextShadow& shadow, qreal zoom,
             const QVector<QRectF>& rects = {}, const QList<TextLayout::FormatRange>& formats = {}) const;

 private:
  struct Owner;
  mutable std::shared_ptr<Owner> owner_;
};

}  // namespace muffin
