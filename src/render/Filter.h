#pragma once

#include <QImage>
#include <QMarginsF>
#include <QRectF>
#include <functional>

class QPainter;

namespace muffin {

struct ThemeElementPaintStyle;

// Apply the ordered CSS filter list to the complete premultiplied element layer.
// Lengths use logical pixels at scale, then the image's device pixel ratio.
void applyElementFilter(QImage& image, const ThemeElementPaintStyle& style, qreal scale = 1);
// CSS `backdrop-filter:` — same pipeline, applied to a sample of the backdrop
// (the content painted behind the box) the caller captured.
void applyElementBackdrop(QImage& image, const ThemeElementPaintStyle& style, qreal scale = 1);
QMarginsF cssFilterOutsets(const ThemeElementPaintStyle& style, qreal scale = 1);
void clearElementFilter(ThemeElementPaintStyle& style);

// True when the style declares any non-default filter / backdrop-filter (so callers
// can skip the offscreen render + composite when nothing would change).
bool hasElementFilter(const ThemeElementPaintStyle& style);
bool hasElementBackdrop(const ThemeElementPaintStyle& style);

struct CssFilterRasterStats {
  quint64 allocatedPixels = 0, largestSurfacePixels = 0, sourcePaints = 0;
};
CssFilterRasterStats cssFilterRasterStats();
void resetCssFilterRasterStats();
// Widget dimensions are logical; image/pixmap dimensions are physical pixels.
// Return the implicit device clip in the painter's current logical space.
QRectF cssPaintDeviceBounds(const QPainter& painter);
// Back-propagate the visible region through the ordered filters. Disjoint
// source/shadow regions never allocate the transparent distance between them.
// Source and finish callbacks use the element's logical coordinate space.
void paintSparseCssFilter(QPainter& painter, QRectF sourceBounds, const ThemeElementPaintStyle& style, qreal scale,
                          const std::function<void(QPainter&)>& source,
                          const std::function<void(QImage&, QPointF)>& finish = {});

}  // namespace muffin
