#include "render/CssBackgroundPainter.h"
#include "diagnostics/ThemePerformance.h"
#include "diagnostics/ScopedPerfProbe.h"

#include "render/GradientPainter.h"
#include "render/LayoutBox.h"
#include "theme/CssImageResources.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

namespace muffin {
namespace {
Q_LOGGING_CATEGORY(maskPerf, "muffin.perf.paint", QtWarningMsg)
using Repeat = ThemeBackgroundLayer::Repeat;
QRectF boxRect(const LayoutBox& box, const QString& name, QPointF offset) {
  return (name == "padding-box" ? box.paddingBox : name == "content-box" ? box.contentBox : box.borderBox).translated(offset);
}

struct Axis {
  qreal position = 0, step = 0;
  bool repeat = false;
};
Axis axisGeometry(Repeat mode, qreal start, qreal extent, qreal tile, qreal position) {
  Axis axis{position, tile, mode != Repeat::NoRepeat};
  if (mode == Repeat::Space) {
    const qreal count = std::floor(extent / tile);
    if (count < 2) axis.repeat = false;
    else { axis.position = start; axis.step = (extent - tile) / (count - 1); }
  }
  if (axis.repeat) axis.position = start + std::fmod(axis.position - start, axis.step);
  return axis;
}

void paintLayer(QPainter& painter, const LayoutBox& box, QPointF offset, const ThemeBackgroundLayer& layer, qreal scale) {
  if(layer.image.kind==GradientSpec::Kind::None && !layer.resource && layer.bitmap.isNull())return;
  const auto area = boxRect(box, layer.origin, offset);
  if (!area.isValid()) return;
  const auto used = [&](const CssLengthPercentage& value, qreal basis, qreal fallback) {
    return value.status == CssLengthStatus::Valid ? value.px * scale + value.fraction * basis : fallback;
  };
  const bool resource = layer.resource || !layer.bitmap.isNull();
  QSizeF intrinsic = layer.resource ? layer.resource->intrinsicSize * scale
                            : !layer.bitmap.isNull() ? QSizeF(layer.bitmap.size()) * scale : area.size();
  const auto ratio = layer.resource ? layer.resource->intrinsicRatio : !layer.bitmap.isNull() ? intrinsic.width() / intrinsic.height() : 0;
  if (layer.resource) {
    const bool width = layer.resource->intrinsicWidth, height = layer.resource->intrinsicHeight;
    if (!width && !height) {
      intrinsic = area.size();
      if (ratio > 0) {
        if (intrinsic.width() / intrinsic.height() > ratio) intrinsic.setWidth(intrinsic.height() * ratio);
        else intrinsic.setHeight(intrinsic.width() / ratio);
      }
    } else if (!width) intrinsic.setWidth(ratio > 0 ? intrinsic.height() * ratio : area.width());
    else if (!height) intrinsic.setHeight(ratio > 0 ? intrinsic.width() / ratio : area.height());
  }
  QSizeF size(used(layer.sizeX, area.width(), intrinsic.width()), used(layer.sizeY, area.height(), intrinsic.height()));
  const bool autoX = layer.sizeX.status != CssLengthStatus::Valid, autoY = layer.sizeY.status != CssLengthStatus::Valid;
  if (resource && ratio > 0 && !autoX && autoY) size.setHeight(size.width() / ratio);
  if (resource && ratio > 0 && !autoY && autoX) size.setWidth(size.height() * ratio);
  if (resource && layer.sizeMode != ThemeBackgroundLayer::SizeMode::Explicit) {
    const auto x = area.width() / intrinsic.width(), y = area.height() / intrinsic.height();
    size = intrinsic * (layer.sizeMode == ThemeBackgroundLayer::SizeMode::Cover ? qMax(x, y) : qMin(x, y));
  }
  if (!std::isfinite(size.width()) || !std::isfinite(size.height()) || size.width() <= 0 || size.height() <= 0) return;
  const QSizeF beforeRound = size;
  if (layer.repeatX == Repeat::Round) size.setWidth(area.width() / qMax(1., std::floor(area.width() / size.width() + .5)));
  if (layer.repeatY == Repeat::Round) size.setHeight(area.height() / qMax(1., std::floor(area.height() / size.height() + .5)));
  // CSS Backgrounds 3 sizing step 4: a single rounded axis rescales the other
  // auto axis to preserve the source's intrinsic ratio. Two round axes stretch.
  if (layer.sizeMode == ThemeBackgroundLayer::SizeMode::Explicit && layer.repeatX == Repeat::Round && layer.repeatY != Repeat::Round && autoY)
    size.setHeight(size.height() * size.width() / beforeRound.width());
  if (layer.sizeMode == ThemeBackgroundLayer::SizeMode::Explicit && layer.repeatY == Repeat::Round && layer.repeatX != Repeat::Round && autoX)
    size.setWidth(size.width() * size.height() / beforeRound.height());
  // Chromium resolves the percentage anchor against the pre-round image size;
  // the rounded period then repeats from that anchor. Retain both geometries.
  const auto x = axisGeometry(layer.repeatX, area.left(), area.width(), size.width(),
                              area.left() + used(layer.positionX, area.width() - beforeRound.width(), 0));
  const auto y = axisGeometry(layer.repeatY, area.top(), area.height(), size.height(),
                              area.top() + used(layer.positionY, area.height() - beforeRound.height(), 0));
  const QPointF position(x.position, y.position);
  const auto clip = boxRect(box, layer.clip, offset);
  painter.save();
  painter.setClipRect(clip, Qt::IntersectClip);
  const qreal dpr = painter.device()->devicePixelRatioF();
  const auto pixelsFor = [&](QSizeF logical) {
    return QSize(int(qBound(1., std::ceil(logical.width() * dpr), 4096.)),
                 int(qBound(1., std::ceil(logical.height() * dpr), 4096.)));
  };
  const auto image = layer.resource ? layer.resource->raster(pixelsFor(size)) : layer.bitmap;
  if (!x.repeat && !y.repeat) {
    const QRectF target(position, size);
    if (resource) painter.drawImage(target, image);
    else painter.fillRect(target, GradientPainter::makeBrush(layer.image, target, scale));
  } else if (!resource && size == area.size() && x.step == size.width() && y.step == size.height()) {
    // Usual full-size gradients avoid a raster allocation, including tall page
    // backgrounds. The clip is at most the surrounding box edges.
    const auto visible = clip.intersected(painter.clipBoundingRect());
    const int firstX = x.repeat ? int(std::floor((visible.left() - x.position) / x.step)) : 0;
    const int lastX = x.repeat ? int(std::floor((visible.right() - x.position) / x.step)) : 0;
    const int firstY = y.repeat ? int(std::floor((visible.top() - y.position) / y.step)) : 0;
    const int lastY = y.repeat ? int(std::floor((visible.bottom() - y.position) / y.step)) : 0;
    for (int row = firstY; row <= lastY; ++row) for (int column = firstX; column <= lastX; ++column) {
      const QRectF target(position + QPointF(column * x.step, row * y.step), size);
      painter.fillRect(target, GradientPainter::makeBrush(layer.image, target, scale));
    }
  } else {
    const QSizeF period(x.repeat ? x.step : size.width(), y.repeat ? y.step : size.height());
    QImage tile = image;
    if (!resource || period != size) {
      const auto pixels = pixelsFor(period);
      tile = QImage(pixels, QImage::Format_ARGB32_Premultiplied); tile.fill(Qt::transparent);
      QPainter buffer(&tile);
      buffer.setRenderHints(painter.renderHints());
      buffer.scale(pixels.width() / period.width(), pixels.height() / period.height());
      const QRectF target(QPointF(), size);
      if (resource) buffer.drawImage(target, image);
      else buffer.fillRect(target, GradientPainter::makeBrush(layer.image, target, scale));
    }
    if (!tile.isNull()) {
      auto fill = clip;
      if (!x.repeat) { fill.setLeft(x.position); fill.setWidth(size.width()); }
      if (!y.repeat) { fill.setTop(y.position); fill.setHeight(size.height()); }
      QBrush brush(tile);
      QTransform transform; transform.translate(position.x(), position.y());
      transform.scale(period.width() / tile.width(), period.height() / tile.height());
      brush.setTransform(transform);
      painter.fillRect(fill, brush);
    }
  }
  painter.restore();
}

void paintColor(QPainter& painter, const LayoutBox& box, QPointF offset, const ThemeElementPaintStyle& style) {
  if (style.backgroundColor.isValid()) painter.fillRect(boxRect(box, style.backgroundColorClip, offset), style.backgroundColor);
}
bool hasImage(const ThemeBackgroundLayer& layer) {
  return GradientPainter::isGradient(layer.image) || layer.resource || !layer.bitmap.isNull();
}
}

void paintCssBackgrounds(QPainter& painter, const LayoutBox& box, QPointF offset, const ThemeElementPaintStyle& style, qreal scale) {
  ThemePerfScope performance(ThemePerfStage::Background);
  const bool blended = std::any_of(style.backgroundLayers.begin(), style.backgroundLayers.end(), [](const auto& layer) {
    return hasImage(layer) && layer.blendMode != CssBlendMode::Normal;
  });
  if (!blended) {
    paintColor(painter, box, offset, style);
    for (auto layer = style.backgroundLayers.crbegin(); layer != style.backgroundLayers.crend(); ++layer)
      if (hasImage(*layer)) paintLayer(painter, box, offset, *layer, scale);
    return;
  }
  // Background blending is isolated from ancestors and from the element's
  // borders/content. Limit the temporary to the visible part of the box.
  const auto visible = painter.worldTransform().inverted().mapRect(QRectF(painter.viewport()));
  auto area = box.borderBox.translated(offset).intersected(visible);
  if (painter.hasClipping()) area = area.intersected(painter.clipBoundingRect());
  if (!area.isValid()) return;
  const qreal dpr = painter.device()->devicePixelRatioF();
  const QSize pixels(qCeil(area.width() * dpr), qCeil(area.height() * dpr));
  QImage background(pixels, QImage::Format_ARGB32_Premultiplied); background.setDevicePixelRatio(dpr); background.fill(Qt::transparent);
  {
    QPainter buffer(&background); buffer.translate(-area.topLeft()); paintColor(buffer, box, offset, style);
  }
  for (auto layer = style.backgroundLayers.crbegin(); layer != style.backgroundLayers.crend(); ++layer) {
    if (!hasImage(*layer)) continue;
    if (const auto composition = cssCompositionMode(layer->blendMode)) {
      QPainter buffer(&background); buffer.setRenderHints(painter.renderHints()); buffer.translate(-area.topLeft());
      buffer.setCompositionMode(*composition); paintLayer(buffer, box, offset, *layer, scale);
    } else {
      QImage source(pixels, QImage::Format_ARGB32_Premultiplied); source.setDevicePixelRatio(dpr); source.fill(Qt::transparent);
      {
        QPainter buffer(&source); buffer.setRenderHints(painter.renderHints()); buffer.translate(-area.topLeft());
        paintLayer(buffer, box, offset, *layer, scale);
      }
      blendNonseparableCssImages(background, source, layer->blendMode);
    }
  }
  painter.drawImage(area.topLeft(), background);
}

void applyCssMasks(QImage& image,QPointF imageOrigin,const LayoutBox& box,const ThemeElementPaintStyle& style,qreal scale) {
  if(style.maskLayers.empty())return;
  diag::ScopedPerfProbe perf("paint.cssMasks", maskPerf());
  QImage mask(image.size(),QImage::Format_ARGB32_Premultiplied);mask.setDevicePixelRatio(image.devicePixelRatioF());mask.fill(Qt::transparent);
  bool first=true;
  for(auto layer=style.maskLayers.crbegin();layer!=style.maskLayers.crend();++layer) {
    if(layer->none)continue;  // Chromium omits explicit none entries, retaining list indices.
    QImage source(image.size(),QImage::Format_ARGB32_Premultiplied);source.setDevicePixelRatio(image.devicePixelRatioF());source.fill(Qt::transparent);
    {QPainter painter(&source);painter.setRenderHint(QPainter::Antialiasing);painter.translate(-imageOrigin);paintLayer(painter,box,{},layer->image,scale);}
    if(layer->luminance)for(int y=0;y<source.height();++y) {
      auto* row=reinterpret_cast<QRgb*>(source.scanLine(y));
      for(int x=0;x<source.width();++x) {const int a=qRound(.2125*qRed(row[x])+.7154*qGreen(row[x])+.0721*qBlue(row[x]));row[x]=qRgba(a,a,a,a);}
    }
    QPainter composite(&mask);
    composite.setCompositionMode(first?QPainter::CompositionMode_Source:layer->composite=="subtract"?QPainter::CompositionMode_SourceOut:
      layer->composite=="intersect"?QPainter::CompositionMode_SourceIn:layer->composite=="exclude"?QPainter::CompositionMode_Xor:QPainter::CompositionMode_SourceOver);
    composite.drawImage(QPointF(),source);first=false;
  }
  QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);painter.drawImage(QPointF(),mask);
}
}  // namespace muffin
