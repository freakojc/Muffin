#include "render/Filter.h"
#include "diagnostics/ScopedPerfProbe.h"

#include <QPainter>
#include <QRegion>
#include <QtMath>
#include <array>
#include <cmath>

#include "render/Blur.h"
#include "theme/ThemeDefinition.h"

namespace muffin {
namespace {
Q_LOGGING_CATEGORY(filterPerf, "muffin.perf.paint", QtWarningMsg)
using K = CssFilterOperation::Kind;
std::vector<CssFilterOperation> legacyFilters(const ThemeElementPaintStyle& s, bool backdrop) {
  const std::array<std::pair<K, qreal>, 7> values{
      {{K::Brightness, backdrop ? s.backdropBrightness : s.filterBrightness},
       {K::Contrast, backdrop ? s.backdropContrast : s.filterContrast},
       {K::Grayscale, backdrop ? s.backdropGrayscale : s.filterGrayscale},
       {K::Sepia, backdrop ? s.backdropSepia : s.filterSepia},
       {K::HueRotate, backdrop ? s.backdropHueRotateDeg : s.filterHueRotateDeg},
       {K::Blur, backdrop ? s.backdropBlur : s.filterBlur},
       {K::Opacity, backdrop ? s.backdropOpacity : s.filterOpacity}}};
  std::vector<CssFilterOperation> result;
  for (const auto& [kind, value] : values)
    if (value != identityCssFilter(kind).value) {
      auto op = identityCssFilter(kind);
      op.value = value;
      result.push_back(op);
    }
  return result;
}
void gaussianBlur(QImage& image, qreal sigma) {
  diag::ScopedPerfProbe perf("paint.filterBlur", filterPerf());
  if (sigma <= 0)
    return;
  if (sigma > 8) {
    boxBlur(image, sigma >= 256 ? 256 : qMax(1, qRound(std::sqrt(sigma * sigma + .25) - .5)));
    return;
  }
  const int radius = qCeil(sigma * 3);
  std::vector<qreal> kernel(2 * radius + 1);
  qreal total = 0;
  for (int i = -radius; i <= radius; ++i)
    total += (kernel[i + radius] = std::exp(-i * i / (2 * sigma * sigma)));
  for (auto& v : kernel)
    v /= total;
  QImage temporary = image.copy();
  for (int axis = 0; axis < 2; ++axis) {
    const auto source = axis ? temporary : image;
    auto& target = axis ? image : temporary;
    for (int y = 0; y < image.height(); ++y)
      for (int x = 0; x < image.width(); ++x) {
        std::array<qreal, 4> sum{};
        for (int i = -radius; i <= radius; ++i) {
          const int sx = x + (axis ? 0 : i), sy = y + (axis ? i : 0);
          if (sx < 0 || sy < 0 || sx >= source.width() || sy >= source.height())
            continue;
          const auto pixel = reinterpret_cast<const QRgb*>(source.constScanLine(sy))[sx];
          const qreal weight = kernel[i + radius];
          sum[0] += qRed(pixel) * weight;
          sum[1] += qGreen(pixel) * weight;
          sum[2] += qBlue(pixel) * weight;
          sum[3] += qAlpha(pixel) * weight;
        }
        reinterpret_cast<QRgb*>(target.scanLine(y))[x] =
            qRgba(qRound(sum[0]), qRound(sum[1]), qRound(sum[2]), qRound(sum[3]));
      }
  }
}
void applyList(QImage& image, const std::vector<CssFilterOperation>& operations, qreal scale) {
  image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
  const qreal pixels = scale * image.devicePixelRatioF();
  for (const auto& op : operations) {
    if (op.kind == K::Blur) {
      gaussianBlur(image, op.value * pixels);
      continue;
    }
    if (op.kind == K::DropShadow) {
      QImage shadow = image.copy();
      QPainter tint(&shadow);
      tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
      tint.fillRect(shadow.rect(), op.color);
      tint.end();
      gaussianBlur(shadow, op.value * pixels);
      QImage result(image.size(), QImage::Format_ARGB32_Premultiplied);
      result.setDevicePixelRatio(image.devicePixelRatioF());
      result.fill(Qt::transparent);
      QPainter painter(&result);
      painter.drawImage(op.offset * scale, shadow);
      painter.drawImage(QPointF(), image);
      painter.end();
      image = std::move(result);
      continue;
    }
    for (int y = 0; y < image.height(); ++y) {
      auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
      for (int x = 0; x < image.width(); ++x) {
        const int alpha = qAlpha(row[x]);
        if (!alpha)
          continue;
        const qreal r = qRed(row[x]) / qreal(alpha), g = qGreen(row[x]) / qreal(alpha),
                    b = qBlue(row[x]) / qreal(alpha), v = op.value;
        qreal red = r, green = g, blue = b, a = alpha;
        switch (op.kind) {
        case K::Brightness:
          red = r * v;
          green = g * v;
          blue = b * v;
          break;
        case K::Contrast:
          red = (r - .5) * v + .5;
          green = (g - .5) * v + .5;
          blue = (b - .5) * v + .5;
          break;
        case K::Grayscale: {
          const qreal l = .2126 * r + .7152 * g + .0722 * b;
          red = r * (1 - v) + l * v;
          green = g * (1 - v) + l * v;
          blue = b * (1 - v) + l * v;
          break;
        }
        case K::Sepia:
          red = r * (1 - v) + (.393 * r + .769 * g + .189 * b) * v;
          green = g * (1 - v) + (.349 * r + .686 * g + .168 * b) * v;
          blue = b * (1 - v) + (.272 * r + .534 * g + .131 * b) * v;
          break;
        case K::Invert:
          red = r * (1 - 2 * v) + v;
          green = g * (1 - 2 * v) + v;
          blue = b * (1 - 2 * v) + v;
          break;
        case K::Opacity:
          a = alpha * v;
          break;
        case K::Saturate:
          red = (.213 + .787 * v) * r + (.715 - .715 * v) * g + (.072 - .072 * v) * b;
          green = (.213 - .213 * v) * r + (.715 + .285 * v) * g + (.072 - .072 * v) * b;
          blue = (.213 - .213 * v) * r + (.715 - .715 * v) * g + (.072 + .928 * v) * b;
          break;
        case K::HueRotate: {
          const auto c = std::cos(v * M_PI / 180), s = std::sin(v * M_PI / 180);
          red = (.213 + .787 * c - .213 * s) * r + (.715 - .715 * c - .715 * s) * g + (.072 - .072 * c + .928 * s) * b;
          green =
              (.213 - .213 * c + .143 * s) * r + (.715 + .285 * c + .140 * s) * g + (.072 - .072 * c - .283 * s) * b;
          blue = (.213 - .213 * c - .787 * s) * r + (.715 - .715 * c + .715 * s) * g + (.072 + .928 * c + .072 * s) * b;
          break;
        }
        default:
          break;
        }
        row[x] = qRgba(qRound(qBound<qreal>(0, red, 1) * a), qRound(qBound<qreal>(0, green, 1) * a),
                       qRound(qBound<qreal>(0, blue, 1) * a), qRound(a));
      }
    }
  }
}
} // namespace
bool hasElementFilter(const ThemeElementPaintStyle& s) {
  return !s.filters.empty() || s.filterBlur != 0 || s.filterBrightness != 1 || s.filterContrast != 1 ||
         s.filterGrayscale != 0 || s.filterSepia != 0 || s.filterHueRotateDeg != 0 || s.filterOpacity != 1;
}
bool hasElementBackdrop(const ThemeElementPaintStyle& s) {
  return !s.backdropFilters.empty() || s.backdropBlur != 0 || s.backdropBrightness != 1 || s.backdropContrast != 1 ||
         s.backdropGrayscale != 0 || s.backdropSepia != 0 || s.backdropHueRotateDeg != 0 || s.backdropOpacity != 1;
}
void applyElementFilter(QImage& image, const ThemeElementPaintStyle& style, qreal scale) {
  applyList(image, style.filters.empty() ? legacyFilters(style, false) : style.filters, scale);
}
void applyElementBackdrop(QImage& image, const ThemeElementPaintStyle& style, qreal scale) {
  diag::ScopedPerfProbe perf("paint.backdropFilter", filterPerf());
  applyList(image, style.backdropFilters.empty() ? legacyFilters(style, true) : style.backdropFilters, scale);
}
QMarginsF cssFilterOutsets(const ThemeElementPaintStyle& style, qreal scale) {
  QMarginsF result;
  if (!hasElementFilter(style))
    return result;
  const auto legacy = style.filters.empty() ? legacyFilters(style, false) : std::vector<CssFilterOperation>{};
  const auto& list = style.filters.empty() ? legacy : style.filters;
  for (const auto& op : list) {
    if (op.kind == K::Blur) {
      const auto v = 3 * op.value * scale;
      result += QMarginsF(v, v, v, v);
    }
    if (op.kind == K::DropShadow) {
      const auto v = 3 * op.value * scale;
      result += QMarginsF(qMax<qreal>(0, v - op.offset.x() * scale), qMax<qreal>(0, v - op.offset.y() * scale),
                          qMax<qreal>(0, v + op.offset.x() * scale), qMax<qreal>(0, v + op.offset.y() * scale));
    }
  }
  return result;
}
void clearElementFilter(ThemeElementPaintStyle& style) {
  style.filters.clear();
  style.filterPresent = false;
  style.filterBlur = style.filterGrayscale = style.filterSepia = style.filterHueRotateDeg = 0;
  style.filterBrightness = style.filterContrast = style.filterOpacity = 1;
}

namespace {
thread_local CssFilterRasterStats rasterStats;
constexpr int tileSize = 512;
struct FilterPatch {
  QRect rect;
  QImage image;
};
using Patches = std::vector<FilterPatch>;
int blurHalo(qreal sigma) {
  return sigma <= 0   ? 0
         : sigma <= 8 ? qCeil(sigma * 3)
                      : 3 * (sigma >= 256 ? 256 : qMax(1, qRound(std::sqrt(sigma * sigma + .25) - .5)));
}
QImage surface(QSize size) {
  diag::ScopedPerfProbe perf("paint.filterSurface", filterPerf());
  QImage image(size, QImage::Format_ARGB32_Premultiplied);
  const auto pixels = quint64(size.width()) * size.height();
  rasterStats.allocatedPixels += pixels;
  rasterStats.largestSurfacePixels = qMax(rasterStats.largestSurfacePixels, pixels);
  image.fill(Qt::transparent);
  return image;
}
QRegion expanded(const QRegion& region, int halo, QPointF offset = {}) {
  QRegion result;
  for (const auto& rect : region)
    result += QRectF(rect).translated(offset).adjusted(-halo, -halo, halo, halo).toAlignedRect();
  return result;
}
void drawPatches(QPainter& painter, const Patches& patches, QPointF origin, QRectF sample) {
  for (const auto& patch : patches)
    if (QRectF(patch.rect).intersects(sample))
      painter.drawImage(QPointF(patch.rect.topLeft()) - origin, patch.image);
}
QImage samplePatches(const Patches& patches, QRect rect) {
  auto image = surface(rect.size());
  QPainter painter(&image);
  drawPatches(painter, patches, rect.topLeft(), rect);
  return image;
}
bool hasPixels(const QImage& image) {
  for (int y = 0; y < image.height(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x = 0; x < image.width(); ++x)
      if (qAlpha(row[x]))
        return true;
  }
  return false;
}
template <class F> void tiles(const QRegion& region, F visit) {
  for (const auto& rect : region)
    for (int y = rect.top(); y <= rect.bottom(); y += tileSize)
      for (int x = rect.left(); x <= rect.right(); x += tileSize)
        visit(QRect(x, y, qMin(tileSize, rect.right() - x + 1), qMin(tileSize, rect.bottom() - y + 1)));
}
} // namespace
CssFilterRasterStats cssFilterRasterStats() {
  return rasterStats;
}
void resetCssFilterRasterStats() {
  rasterStats = {};
}
QRectF cssPaintDeviceBounds(const QPainter& painter) {
  bool invertible = false;
  const auto inverse = painter.deviceTransform().inverted(&invertible);
  if (!invertible)
    return {};
  const auto* device = painter.device();
  const auto units = device->devType() == QInternal::Widget ? device->devicePixelRatioF() : 1;
  return inverse.mapRect(QRectF(0, 0, device->width() * units, device->height() * units));
}
void paintSparseCssFilter(QPainter& painter, QRectF sourceBounds, const ThemeElementPaintStyle& style, qreal scale,
                          const std::function<void(QPainter&)>& source,
                          const std::function<void(QImage&, QPointF)>& finish) {
  diag::ScopedPerfProbe perf("paint.sparseFilter", filterPerf());
  const auto device = painter.device();
  auto visible = cssPaintDeviceBounds(painter);
  if (painter.hasClipping())
    visible = visible.intersected(painter.clipBoundingRect());
  if (visible.isEmpty())
    return;
  const qreal dpr = device->devicePixelRatioF();
  // Anchor near the viewport, so a large document scroll doesn't consume the
  // integer region coordinate range. Raster boundaries stay device aligned.
  const QPointF anchor(std::floor(visible.left() * dpr) / dpr, std::floor(visible.top() * dpr) / dpr);
  auto operations = style.filters.empty() ? legacyFilters(style, false) : style.filters;
  for (auto& op : operations) {
    if (op.kind == K::Blur || op.kind == K::DropShadow)
      op.value *= scale * dpr;
    if (op.kind == K::DropShadow)
      op.offset *= scale * dpr;
  }
  std::vector<QRegion> demand(operations.size() + 1);
  const auto physical = [&](QRectF rect) {
    rect.translate(-anchor);
    return QRectF(rect.topLeft() * dpr, rect.size() * dpr).toAlignedRect();
  };
  demand.back() = physical(visible);
  for (size_t i = operations.size(); i-- > 0;) {
    const auto& op = operations[i];
    demand[i] = demand[i + 1];
    if (op.kind == K::Blur)
      demand[i] = expanded(demand[i + 1], blurHalo(op.value));
    if (op.kind == K::DropShadow && op.color.alpha() != 0)
      demand[i] += expanded(demand[i + 1], blurHalo(op.value) + 1, -op.offset);
  }
  Patches patches;
  tiles(demand.front().intersected(physical(sourceBounds)), [&](QRect rect) {
    const QRectF logical(anchor + QPointF(rect.topLeft()) / dpr, QSizeF(rect.size()) / dpr);
    if (!logical.intersects(sourceBounds))
      return;
    auto image = surface(rect.size());
    image.setDevicePixelRatio(dpr);
    {
      QPainter buffer(&image);
      buffer.setRenderHints(painter.renderHints());
      buffer.translate(-logical.topLeft());
      ++rasterStats.sourcePaints;
      diag::ScopedPerfProbe sourcePerf("paint.filterSource", filterPerf());
      source(buffer);
    }
    image.setDevicePixelRatio(1);
    if (hasPixels(image))
      patches.push_back({rect, std::move(image)});
  });
  for (size_t i = 0; i < operations.size() && !patches.empty(); ++i) {
    const auto& op = operations[i];
    Patches next;
    QRegion support;
    for (const auto& patch : patches)
      support += patch.rect;
    if (op.kind == K::Blur)
      support = expanded(support, blurHalo(op.value));
    if (op.kind == K::DropShadow && op.color.alpha() != 0)
      support += expanded(support, blurHalo(op.value) + 1, op.offset);
    tiles(demand[i + 1].intersected(support), [&](QRect rect) {
      auto image = samplePatches(patches, rect);
      if (op.kind == K::Blur) {
        const auto area =
            rect.adjusted(-blurHalo(op.value), -blurHalo(op.value), blurHalo(op.value), blurHalo(op.value));
        auto blurred = samplePatches(patches, area);
        gaussianBlur(blurred, op.value);
        image = blurred.copy(QRect(rect.topLeft() - area.topLeft(), rect.size()));
      } else if (op.kind == K::DropShadow && op.color.alpha() != 0) {
        diag::ScopedPerfProbe shadowPerf("paint.filterDropShadow", filterPerf());
        const int halo = blurHalo(op.value) + 1;
        const auto area = QRectF(rect).translated(-op.offset).adjusted(-halo, -halo, halo, halo).toAlignedRect();
        auto shadow = samplePatches(patches, area);
        {
          QPainter tint(&shadow);
          tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
          tint.fillRect(shadow.rect(), op.color);
        }
        gaussianBlur(shadow, op.value);
        auto combined = surface(rect.size());
        QPainter target(&combined);
        target.setRenderHint(QPainter::SmoothPixmapTransform);
        target.drawImage(QPointF(area.topLeft()) + op.offset - QPointF(rect.topLeft()), shadow);
        target.drawImage(QPointF(), image);
        target.end();
        image = std::move(combined);
      } else if (op.kind != K::DropShadow)
        applyList(image, {op}, 1);
      if (hasPixels(image))
        next.push_back({rect, std::move(image)});
    });
    patches = std::move(next);
  }
  for (auto& patch : patches) {
    const auto origin = anchor + QPointF(patch.rect.topLeft()) / dpr;
    patch.image.setDevicePixelRatio(dpr);
    if (finish)
      finish(patch.image, origin);
    painter.drawImage(origin, patch.image);
  }
}
} // namespace muffin
