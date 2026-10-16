#pragma once

#include <QPointF>

class QPainter;
class QImage;
namespace muffin {
struct LayoutBox;
struct ThemeElementPaintStyle;
// Paint only the isolated background stack. Borders, shadows and content stay
// outside background-blend-mode's group.
void paintCssBackgrounds(QPainter& painter, const LayoutBox& box, QPointF offset,
                         const ThemeElementPaintStyle& style, qreal scale);
void applyCssMasks(QImage& image, QPointF imageOrigin, const LayoutBox& box,
                    const ThemeElementPaintStyle& style, qreal scale);
}
