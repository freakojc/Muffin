#include "render/LayoutBox.h"

#include "render/DecorationPainter.h"
#include "render/GradientPainter.h"
#include "render/Filter.h"
#include "render/CssPaintAnimation.h"
#include <QPainter>
#include <QPainterPath>
#include <cmath>

namespace muffin {

QTransform elementPaintTransform(const ThemeElementPaintStyle& style, const QRectF& rect, qreal zoom) {
  QTransform transform;
  transform.translate(rect.center().x(), rect.center().y());
  for (const auto& op : style.transforms) {
    using Kind = ThemeElementPaintStyle::Transform::Kind;
    if (op.kind == Kind::Translate)
      transform.translate(op.x.px * zoom + op.x.fraction * rect.width(), op.y.px * zoom + op.y.fraction * rect.height());
    else if (op.kind == Kind::Scale) transform.scale(op.x.px, op.y.px);
    else transform.rotate(op.angle);
  }
  transform.translate(-rect.center().x(), -rect.center().y());
  return transform;
}

void paintCssElement(QPainter& painter, QRectF borderBox, QRectF visualBounds, const ThemeElementPaintStyle& style,
                     qreal zoom, CssPaintContext* context,
                     const std::function<void(QPainter&, const ThemeElementPaintStyle&)>& contents) {
  painter.save();
  painter.setWorldTransform(elementPaintTransform(style, borderBox, zoom), true);
  const bool filtered = hasElementFilter(style);
  if (!filtered && style.opacity == 1) contents(painter, style);
  else {
    const qreal padding = style.filterBlur + 2;
    const auto visible = painter.worldTransform().inverted().mapRect(QRectF(painter.viewport()));
    auto area = visualBounds.adjusted(-padding, -padding, padding, padding).intersected(visible.adjusted(-padding, -padding, padding, padding));
    if (painter.hasClipping()) area = area.intersected(painter.clipBoundingRect().adjusted(-padding, -padding, padding, padding));
    if (area.isValid()) {
      const qreal dpr = painter.device()->devicePixelRatioF();
      QImage layer(qCeil(area.width() * dpr), qCeil(area.height() * dpr), QImage::Format_ARGB32_Premultiplied);
      layer.setDevicePixelRatio(dpr); layer.fill(Qt::transparent);
      auto unfiltered = style;
      unfiltered.opacity = 1; unfiltered.filterPresent = false; unfiltered.filterBlur = 0;
      unfiltered.filterBrightness = unfiltered.filterContrast = unfiltered.filterOpacity = 1;
      unfiltered.filterGrayscale = unfiltered.filterSepia = unfiltered.filterHueRotateDeg = 0;
      {
        QPainter buffer(&layer); buffer.setRenderHints(painter.renderHints()); buffer.translate(-area.topLeft());
        const auto pointer = context ? context->pointer : std::nullopt;
        if (pointer) context->pointer = buffer.worldTransform().map(painter.worldTransform().inverted().map(*pointer));
        contents(buffer, unfiltered);
        if (context) context->pointer = pointer;
      }
      if (filtered) applyElementFilter(layer, style);
      painter.setOpacity(painter.opacity() * style.opacity);
      painter.drawImage(area.topLeft(), layer);
    }
  }
  painter.restore();
}

namespace {
void paintBackgroundImage(QPainter& painter, const LayoutBox& box, QPointF offset, const ThemeElementPaintStyle& paint, qreal scale) {
  const auto area = (paint.backgroundOrigin == "border-box" ? box.borderBox
                     : paint.backgroundOrigin == "content-box" ? box.contentBox : box.paddingBox).translated(offset);
  const auto used = [&](const CssLengthPercentage& value, qreal basis, qreal fallback) {
    return value.status == CssLengthStatus::Valid ? value.px * scale + value.fraction * basis : fallback;
  };
  const QSizeF size(used(paint.backgroundSizeX, area.width(), area.width()),
                    used(paint.backgroundSizeY, area.height(), area.height()));
  if (size.width() <= 0 || size.height() <= 0) return;
  QPointF position = area.topLeft() + QPointF(used(paint.backgroundPositionX, area.width() - size.width(), 0),
                                                   used(paint.backgroundPositionY, area.height() - size.height(), 0));
  if (paint.backgroundRepeatX) position.setX(area.left() + std::fmod(position.x() - area.left(), size.width()));
  if (paint.backgroundRepeatY) position.setY(area.top() + std::fmod(position.y() - area.top(), size.height()));
  const auto clipBox = (paint.backgroundClip == "padding-box" ? box.paddingBox
                        : paint.backgroundClip == "content-box" ? box.contentBox : box.borderBox).translated(offset);
  painter.save();
  painter.setClipRect(clipBox, Qt::IntersectClip);
  if (!paint.backgroundRepeatX && !paint.backgroundRepeatY) {
    const QRectF image(position, size);
    painter.fillRect(image, GradientPainter::makeBrush(paint.backgroundImage, image, scale));
  } else if (size == area.size()) {
    // The usual full-size gradient needs no intermediate image.
    for (int y = paint.backgroundRepeatY ? -1 : 0; y <= (paint.backgroundRepeatY ? 1 : 0); ++y)
      for (int x = paint.backgroundRepeatX ? -1 : 0; x <= (paint.backgroundRepeatX ? 1 : 0); ++x) {
        const QRectF image(position + QPointF(x * size.width(), y * size.height()), size);
        painter.fillRect(image, GradientPainter::makeBrush(paint.backgroundImage, image, scale));
      }
  } else {
    // A texture brush repeats in the paint engine; tiny stitches do not create
    // thousands of draw calls. Retain the exact logical period after rounding
    // raster dimensions and render at the target device's pixel density.
    const qreal dpr = painter.device()->devicePixelRatioF();
    const QSize pixels(qBound(1, qCeil(size.width() * dpr), 4096), qBound(1, qCeil(size.height() * dpr), 4096));
    QImage tile(pixels, QImage::Format_ARGB32_Premultiplied);
    tile.fill(Qt::transparent);
    {
      QPainter buffer(&tile);
      buffer.scale(pixels.width() / size.width(), pixels.height() / size.height());
      buffer.fillRect(QRectF(QPointF(), size), GradientPainter::makeBrush(paint.backgroundImage, QRectF(QPointF(), size), scale));
    }
    auto fill = clipBox;
    if (!paint.backgroundRepeatX) { fill.setLeft(position.x()); fill.setWidth(size.width()); }
    if (!paint.backgroundRepeatY) { fill.setTop(position.y()); fill.setHeight(size.height()); }
    QBrush brush(tile);
    QTransform transform; transform.translate(position.x(), position.y());
    transform.scale(size.width() / pixels.width(), size.height() / pixels.height());
    brush.setTransform(transform);
    painter.fillRect(fill, brush);
  }
  painter.restore();
}
}  // namespace

void paintLayoutBox(QPainter& painter, const LayoutBox& box, QPointF offset, const ThemeElementStyle* overrideStyle) {
  const auto rect = box.borderBox.translated(offset);
  if (!rect.isValid()) return;
  auto used = box.usedBox;
  if (overrideStyle) {
    const qreal scale = box.style.text.fontSizePx > 0 ? box.font.pointSizeF() * 96 / 72 / box.style.text.fontSizePx : 1;
    used.borderRadius = overrideStyle->box.borderRadius * scale;
    used.borderTopColor = overrideStyle->box.borderTopColor; used.borderRightColor = overrideStyle->box.borderRightColor;
    used.borderBottomColor = overrideStyle->box.borderBottomColor; used.borderLeftColor = overrideStyle->box.borderLeftColor;
  }
  const auto& paint = overrideStyle ? overrideStyle->paint : box.style.paint;
  if (hasElementBackdrop(paint)) {
    if (auto* device = dynamic_cast<QImage*>(painter.device())) {
      const auto area = painter.transform().mapRect(rect).toAlignedRect().intersected(device->rect());
      if (!area.isEmpty()) {
        auto backdrop = device->copy(area);
        applyElementBackdrop(backdrop, paint);
        painter.save();
        painter.setWorldTransform({});
        painter.setClipRect(area);
        painter.drawImage(area.topLeft(), backdrop);
        painter.restore();
      }
    }
  }
  if (hasElementFilter(paint)) {
    const int padding = qCeil(paint.filterBlur + 2);
    const auto area = rect.adjusted(-padding, -padding, padding, padding);
    QImage image(qCeil(area.width()), qCeil(area.height()), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    auto unfiltered = box;
    if (overrideStyle) unfiltered.style = *overrideStyle;
    unfiltered.style.paint.filterPresent = false;
    unfiltered.style.paint.filterBlur = 0;
    unfiltered.style.paint.filterBrightness = unfiltered.style.paint.filterContrast = unfiltered.style.paint.filterOpacity = 1;
    unfiltered.style.paint.filterGrayscale = unfiltered.style.paint.filterSepia = unfiltered.style.paint.filterHueRotateDeg = 0;
    unfiltered.style.paint.backdropPresent = false;
    unfiltered.style.paint.backdropBlur = 0;
    unfiltered.style.paint.backdropBrightness = unfiltered.style.paint.backdropContrast = unfiltered.style.paint.backdropOpacity = 1;
    unfiltered.style.paint.backdropGrayscale = unfiltered.style.paint.backdropSepia = unfiltered.style.paint.backdropHueRotateDeg = 0;
    {
      QPainter buffer(&image);
      buffer.setRenderHint(QPainter::Antialiasing);
      paintLayoutBox(buffer, unfiltered, offset - area.topLeft());
    }
    applyElementFilter(image, paint);
    painter.drawImage(area.topLeft(), image);
    return;
  }
  painter.save();
  painter.setOpacity(painter.opacity() * paint.opacity);
  DecorationPainter::paintBoxShadow(painter, rect, used.borderRadius, paint.boxShadowColor, paint.boxShadowOffsetX, paint.boxShadowOffsetY,
                                    paint.boxShadowBlur, paint.boxShadowSpread);
  QPainterPath clip;
  clip.addRoundedRect(rect, used.borderRadius, used.borderRadius);
  painter.setClipPath(clip, Qt::IntersectClip);
  painter.setPen(Qt::NoPen);
  if (paint.backgroundColor.isValid()) {
    painter.save();
    if (paint.backgroundClip == "padding-box") painter.setClipRect(box.paddingBox.translated(offset), Qt::IntersectClip);
    if (paint.backgroundClip == "content-box") painter.setClipRect(box.contentBox.translated(offset), Qt::IntersectClip);
    painter.fillPath(clip, paint.backgroundColor);
    painter.restore();
  }
  if (GradientPainter::isGradient(paint.backgroundImage)) {
    const qreal scale = box.style.text.fontSizePx > 0 ? box.font.pointSizeF() * 96 / 72 / box.style.text.fontSizePx : 1;
    paintBackgroundImage(painter, box, offset, paint, scale);
  }
  const auto side = [&](QRectF area, const QColor& color, const QString& style, bool horizontal) {
    if (area.width() <= 0 || area.height() <= 0 || !color.isValid()) return;
    const qreal width = horizontal ? area.height() : area.width();
    if (style == QLatin1String("dashed") || style == QLatin1String("dotted")) {
      painter.save();
      painter.setClipRect(area, Qt::IntersectClip);
      painter.setPen(QPen(color, width, style == QLatin1String("dotted") ? Qt::DotLine : Qt::DashLine, Qt::FlatCap));
      painter.drawLine(horizontal ? QLineF(area.left(), area.center().y(), area.right(), area.center().y())
                                  : QLineF(area.center().x(), area.top(), area.center().x(), area.bottom()));
      painter.restore();
    } else if (style == QLatin1String("double") && width >= 3) {
      const qreal strip = width / 3;
      painter.fillRect(
          horizontal ? QRectF(area.left(), area.top(), area.width(), strip) : QRectF(area.left(), area.top(), strip, area.height()), color);
      painter.fillRect(horizontal ? QRectF(area.left(), area.bottom() - strip, area.width(), strip)
                                  : QRectF(area.right() - strip, area.top(), strip, area.height()),
                       color);
    } else
      painter.fillRect(area, color);
  };
  side(QRectF(rect.left(), rect.top(), rect.width(), used.borderTopWidth), used.borderTopColor, used.borderTopStyle, true);
  side(QRectF(rect.left(), rect.bottom() - used.borderBottomWidth, rect.width(), used.borderBottomWidth), used.borderBottomColor,
       used.borderBottomStyle, true);
  side(QRectF(rect.left(), rect.top(), used.borderLeftWidth, rect.height()), used.borderLeftColor, used.borderLeftStyle, false);
  side(QRectF(rect.right() - used.borderRightWidth, rect.top(), used.borderRightWidth, rect.height()), used.borderRightColor,
       used.borderRightStyle, false);
  painter.restore();
}

}  // namespace muffin
