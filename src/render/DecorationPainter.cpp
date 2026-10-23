#include "render/DecorationPainter.h"
#include "diagnostics/ThemePerformance.h"
#include "diagnostics/ScopedPerfProbe.h"

#include "render/Blur.h"
#include "theme/CssThemeMapper.h"
#include "theme/CssComputedStyleEngine.h"
#include "theme/CssValueParser.h"
#include "render/Filter.h"
#include "render/GradientPainter.h"
#include "render/GeneratedContent.h"
#include "theme/ThemeDefinition.h"

#include <QBrush>
#include <QCache>
#include "render/TextLayout.h"
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QTransform>
#include <QRectF>
#include <QRegularExpression>
#include <QSvgRenderer>
#include <QtMath>

#include <memory>
#include <cmath>
#include <algorithm>
#include <cstring>

namespace muffin {
namespace DecorationPainter {

namespace {
Q_LOGGING_CATEGORY(effectPerf, "muffin.perf.paint", QtWarningMsg)

struct ShadowRasterKey {
  QRectF core;
  QSize size;
  qreal radius;
  int blur;
  QColor color;
  bool operator==(const ShadowRasterKey&) const = default;
  friend size_t qHash(const ShadowRasterKey& key, size_t seed = 0) {
    return qHashMulti(seed, key.core.x(), key.core.y(), key.core.width(), key.core.height(), key.size.width(),
                      key.size.height(), key.radius, key.blur, key.color.rgba());
  }
};
thread_local QCache<ShadowRasterKey, QImage> shadows(16 * 1024);
thread_local BoxShadowRasterStats boxStats;

// A rounded rectangle's middle columns/rows are identical once outside the
// corners and the complete convolution support. Keep one of each, convolve
// the compact mask, then restore those constant runs without interpolation.
// The final image/cache and the caller's one-image compositing stay unchanged.
struct ConstantRun {
  int start = 0, removed = 0;
  int source(int position) const {
    return position < start ? position : position < start + removed + 1 ? start : position - removed;
  }
};
ConstantRun constantRun(qreal first, qreal last, int extent) {
  const int begin = qBound(0, qCeil(first), extent);
  const int end = qBound(begin, qFloor(last), extent);
  return {begin, qMax(0, end - begin - 1)};
}
QImage rasterizeShadow(const ShadowRasterKey& key) {
  const qreal guardX = qMin(key.radius, key.core.width() / 2) + 3 * key.blur + 2;
  const qreal guardY = qMin(key.radius, key.core.height() / 2) + 3 * key.blur + 2;
  const auto x = constantRun(key.core.left() + guardX, key.core.right() - guardX, key.size.width());
  const auto y = constantRun(key.core.top() + guardY, key.core.bottom() - guardY, key.size.height());
  const QSize compactSize(key.size.width() - x.removed, key.size.height() - y.removed);
  if (effectPerf().isDebugEnabled())
    qCDebug(effectPerf).noquote().nospace()
        << "paint.boxCompact " << compactSize.width() << ' ' << compactSize.height();
  boxStats.maskPixels += quint64(compactSize.width()) * compactSize.height();
  boxStats.largestMaskPixels = qMax(boxStats.largestMaskPixels, quint64(compactSize.width()) * compactSize.height());
  boxStats.outputPixels += quint64(key.size.width()) * key.size.height();
  QImage compact(compactSize, QImage::Format_ARGB32_Premultiplied);
  if (compact.isNull())
    return {};
  compact.fill(Qt::transparent);
  {
    diag::ScopedPerfProbe perf("paint.boxMask", effectPerf());
    QPainter painter(&compact);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(key.color);
    painter.drawRoundedRect(QRectF(key.core.topLeft(), key.core.size() - QSizeF(x.removed, y.removed)), key.radius,
                            key.radius);
  }
  countThemePerformanceWork(ThemePerfStage::Shadow, quint64(compact.width()) * compact.height());
  {
    diag::ScopedPerfProbe perf("paint.boxBlur", effectPerf());
    boxBlur(compact, key.blur);
  }
  if (!x.removed && !y.removed)
    return compact;
  diag::ScopedPerfProbe perf("paint.boxExpand", effectPerf());
  QImage result(key.size, QImage::Format_ARGB32_Premultiplied);
  if (result.isNull())
    return {};
  for (int row = 0; row < result.height(); ++row) {
    auto* out = reinterpret_cast<QRgb*>(result.scanLine(row));
    const auto* in = reinterpret_cast<const QRgb*>(compact.constScanLine(y.source(row)));
    if (!x.removed)
      std::memcpy(out, in, size_t(result.width()) * sizeof(QRgb));
    else {
      std::memcpy(out, in, size_t(x.start) * sizeof(QRgb));
      std::fill_n(out + x.start, x.removed + 1, in[x.start]);
      std::memcpy(out + x.start + x.removed + 1, in + x.start + 1,
                  size_t(compact.width() - x.start - 1) * sizeof(QRgb));
    }
  }
  return result;
}

// Process-wide SVG cache keyed by SVG byte data (content-addressed; themes share).
std::shared_ptr<QSvgRenderer> svgIcon(const QByteArray& data) {
  static QHash<QByteArray, std::shared_ptr<QSvgRenderer>> cache;
  if (data.isEmpty()) {
    return nullptr;
  }
  const auto it = cache.constFind(data);
  if (it != cache.constEnd()) {
    return *it;
  }
  auto r = std::make_shared<QSvgRenderer>(data);
  if (!r->isValid()) {
    r.reset();
  }
  cache.insert(data, r);
  return r;
}

// Render an SVG as an alpha mask tinted with `tint`, into a tile of `size` (px).
// Mask semantics: the SVG's alpha (its shape) becomes the tint's coverage, so a
// `mask-image: url(svg)` declaration paints the SVG shape in the ::before's
// background-colour. Returns a null image when the SVG is invalid or the size is
// degenerate. Shared by paintIcon (icon recolour) and paintWriteTexture (page
// texture tiling) — both are the same "alpha mask + tint" recipe at heart.
QImage renderMaskTile(const QByteArray& svgData, const QColor& tint, QSize size) {
  const auto icon = svgIcon(svgData);
  if (!icon || !tint.isValid() || size.width() <= 0 || size.height() <= 0) {
    return QImage();
  }
  QImage shape(size.width(), size.height(), QImage::Format_ARGB32_Premultiplied);
  shape.fill(Qt::transparent);
  {
    QPainter sp(&shape);
    sp.setRenderHint(QPainter::SmoothPixmapTransform, true);
    icon->render(&sp, QRectF(0, 0, size.width(), size.height()));
  }
  QImage out(size.width(), size.height(), QImage::Format_ARGB32_Premultiplied);
  out.fill(Qt::transparent);
  {
    QPainter op(&out);
    op.fillRect(out.rect(), tint);
    op.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    op.drawImage(0, 0, shape);
  }
  return out;
}

const ElementBackground* elementBackground(const RenderTheme& theme, const QString& host) {
  for (const ElementBackground& eb : theme.decorations().backgrounds) {
    if (eb.host == host) {
      return &eb;
    }
  }
  return nullptr;
}

const HoverEffect* hoverEffectFor(const RenderTheme& theme, const QString& host) {
  for (const HoverEffect& he : theme.decorations().hoverEffects) {
    if (he.host == host) {
      return &he;
    }
  }
  return nullptr;
}

} // namespace

BoxShadowRasterStats boxShadowRasterStats() {
  auto result = boxStats;
  result.cacheBytes = shadows.totalCost() * 1024;
  result.cacheEntries = shadows.size();
  return result;
}
void resetBoxShadowRasterStats() {
  boxStats = {};
}

void paintIcon(QPainter& painter, const QByteArray& svgData, const QRectF& target, const QColor& tint, bool recolour) {
  const auto icon = svgIcon(svgData);
  if (!icon) {
    return;
  }
  if (!recolour || !tint.isValid()) {
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    icon->render(&painter, target);
    painter.restore();
    return;
  }
  // Mask recolour: render the SVG as an alpha mask tinted with `tint`, then blit.
  // (phycat's mask icons carry no fill of their own — only shape.)
  const QSize size(qMax(1, int(qCeil(target.width()))), qMax(1, int(qCeil(target.height()))));
  const QImage tile = renderMaskTile(svgData, tint, size);
  if (tile.isNull()) {
    return;
  }
  painter.save();
  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(target, tile);
  painter.restore();
}

bool hasElementBackground(const RenderTheme& theme, const QString& host) {
  const ElementBackground* eb = elementBackground(theme, host);
  return eb && eb->gradient.kind != GradientSpec::Kind::None;
}

void paintHrGradient(QPainter& painter, const RenderTheme& theme, const QRectF& rect) {
  const ElementBackground* eb = elementBackground(theme, QStringLiteral("hr"));
  if (!eb) {
    return;
  }
  const qreal h = qMax<qreal>(2.0, rect.height() * 0.08);
  const QRectF bar(rect.left(), rect.center().y() - h / 2.0, rect.width(), h);
  painter.save();
  painter.fillRect(bar, GradientPainter::makeBrush(eb->gradient, bar, theme.zoomPercent() / 100.0));
  painter.restore();
}

void paintWriteTexture(QPainter& painter, const RenderTheme& theme, const QRectF& pageRect,
                       const std::optional<PseudoElementRule>& rule) {
  if (!rule) {
    return;
  }
  if (rule->computed &&
      (rule->computed->resolvedValue("content") == "none" || rule->computed->resolvedValue("content") == "normal" ||
       rule->computed->resolvedValue("display") == "none"))
    return;
  if (rule->computed) {
    const auto generated = generatedContentStyle(theme, *rule, theme.paragraphFont(), pageRect.size());
    if (!generated)
      return;
    const auto rect = positionGeneratedContent(*generated, pageRect,
                                               QRectF(pageRect.topLeft(), QSizeF(generated->width, generated->height)),
                                               theme.zoomPercent() / 100., false);
    const auto box = LayoutBox::place("#write::before", generated->style, generated->used, rect, generated->font);
    paintCssElement(
        painter, rect, box.visualOverflow, box.style.paint, theme.zoomPercent() / 100., nullptr,
        [&](QPainter& target, const auto& neutral) {
          auto style = box.style;
          style.paint = neutral;
          paintLayoutBox(target, box, {}, &style);
        },
        &box);
    return;
  }
  // A #write::before texture is a MASK — either a gradient mask (maskPattern) or
  // an SVG url() mask (svgData). Both supply shape; the ::before background-colour
  // (maskTint) supplies the visible colour, painted at the rule's opacity. The old
  // code only handled the gradient case and dropped url(svg) masks entirely
  // (phycat's diamond/cross grid), leaving the page blank.
  const bool hasGradientMask = rule->maskPattern.kind != GradientSpec::Kind::None;
  const bool hasSvgMask = !rule->svgData.isEmpty();
  if (!hasGradientMask && !hasSvgMask) {
    return;
  }
  const QColor tint = rule->maskTint.isValid() ? rule->maskTint : theme.textColor();
  const qreal scale = theme.zoomPercent() / 100.0;
  const qreal tileW = qBound(2.0, rule->maskTile.width() * scale, 256.0);
  const qreal tileH = qBound(2.0, rule->maskTile.height() * scale, 256.0);
  QImage tile;
  if (hasGradientMask) {
    // Recolour the mask gradient stops to the tint (a mask is colour-agnostic).
    GradientSpec tinted = rule->maskPattern;
    for (GradientStop& s : tinted.stops) {
      if (s.color != QColor(Qt::transparent)) {
        s.color = tint;
      }
    }
    tile = QImage(int(qCeil(tileW)), int(qCeil(tileH)), QImage::Format_ARGB32_Premultiplied);
    tile.fill(Qt::transparent);
    {
      QPainter tp(&tile);
      tp.fillRect(tile.rect(), GradientPainter::makeBrush(tinted, QRectF(0, 0, tileW, tileH), scale));
    }
  } else {
    tile = renderMaskTile(rule->svgData, tint, QSize(int(qCeil(tileW)), int(qCeil(tileH))));
  }
  if (tile.isNull()) {
    return;
  }
  QBrush pattern(QPixmap::fromImage(tile)); // QBrush(QPixmap) → TexturePattern, tiles
  painter.save();
  painter.setOpacity(rule->opacity);
  painter.fillRect(pageRect, pattern);
  painter.restore();
}

void paintGlow(QPainter& painter, const QRectF& rect, const QColor& color, qreal blur, qreal alpha) {
  if (!color.isValid() || blur <= 0.0 || alpha <= 0.0) {
    return;
  }
  painter.save();
  painter.setPen(Qt::NoPen);
  QColor base = color;
  base.setAlpha(qMin(base.alpha(), 60)); // cap the peak so the shell sum stays soft
  constexpr int kLayers = 8;
  for (int i = kLayers; i >= 1; --i) {
    const qreal grow = blur * (i / qreal(kLayers));
    QColor shell = base;
    shell.setAlphaF(base.alphaF() * alpha / qreal(kLayers));
    painter.setBrush(shell);
    painter.drawRoundedRect(rect.adjusted(-grow, -grow, grow, grow), grow, grow);
  }
  painter.restore();
}

void paintBoxShadow(QPainter& painter, const QRectF& rect, qreal borderRadius, const QColor& color, qreal offsetX,
                    qreal offsetY, qreal blur, qreal spread) {
  ThemePerfScope performance(ThemePerfStage::Shadow);
  diag::ScopedPerfProbe boxPerf("paint.boxShadow", effectPerf());
  if (!color.isValid()) {
    return;
  }
  if (qFuzzyIsNull(offsetX) && qFuzzyIsNull(offsetY) && qFuzzyIsNull(blur) && qFuzzyIsNull(spread)) {
    return;
  }

  const QRectF core = rect.translated(offsetX, offsetY).adjusted(-spread, -spread, spread, spread);
  if (core.isEmpty()) {
    return;
  }

  const qreal clampedBlur = qMax<qreal>(0.0, blur);
  const qreal coreRadius = qMax<qreal>(0.0, borderRadius + spread);
  if (clampedBlur <= 0.0) {
    painter.save();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(core, coreRadius, coreRadius);
    painter.restore();
    return;
  }

  // CSS blurs the spread-expanded shape itself; drawing an opaque spread core
  // after translucent shells creates a hard band outside the page. Render the
  // shape into an alpha image and blur it instead. Three box passes approximate
  // a Gaussian, matching the text/filter blur path already used by Muffin.
  const int blurRadius = qMax(1, qRound(clampedBlur / 2.0));
  const qreal blurExtent = blurRadius * 3.0 + 2.0;
  // Raster coverage has finite subpixel precision. Equivalent eager/lazy sums
  // can differ by 1e-13 at a coverage boundary; normalize only the raster mask,
  // without changing the shared layout/caret geometry or relaxing pixel tests.
  const auto snap = [](qreal value) { return std::round(value * 256) / 256; };
  const QRectF rasterCore(QPointF(snap(core.left()), snap(core.top())),
                          QPointF(snap(core.right()), snap(core.bottom())));
  QRectF paintBounds = rasterCore.adjusted(-blurExtent, -blurExtent, blurExtent, blurExtent);
  // An implicit device clip is not included in clipBoundingRect(). A page
  // shadow must allocate only the visible strip, even in a very tall document.
  const auto visible = cssPaintDeviceBounds(painter);
  paintBounds = paintBounds.intersected(visible.adjusted(-blurExtent, -blurExtent, blurExtent, blurExtent));
  if (painter.hasClipping()) {
    paintBounds =
        paintBounds.intersected(painter.clipBoundingRect().adjusted(-blurExtent, -blurExtent, blurExtent, blurExtent));
  }
  const QRect imageBounds = paintBounds.toAlignedRect();
  if (imageBounds.isEmpty()) {
    return;
  }

  // Translation-independent, exact raster inputs. The visible region and
  // subpixel phase remain part of the key, so scrolling/clipping cannot reuse
  // a different edge mask. Bound retained surfaces to 16 MiB per paint thread.
  // Lazy page height can change while offscreen blocks acquire real geometry.
  // Edges beyond this raster and its corner guard cannot affect any mask pixel;
  // normalize them so a distant page bottom does not evict the visible shadow.
  const qreal cornerGuard = coreRadius * 2 + 4;
  const QRectF localCoverage(-cornerGuard, -cornerGuard, imageBounds.width() + 2 * cornerGuard,
                             imageBounds.height() + 2 * cornerGuard);
  const ShadowRasterKey key{rasterCore.translated(-imageBounds.topLeft()).intersected(localCoverage),
                            imageBounds.size(), coreRadius, blurRadius, color};
  QImage shadow;
  ++boxStats.requests;
  const auto* cached = shadows.object(key);
  if (effectPerf().isDebugEnabled())
    qCDebug(effectPerf).noquote().nospace()
        << "paint.boxRaster " << imageBounds.width() << ' ' << imageBounds.height() << ' ' << blurRadius << ' '
        << key.core.x() << ' ' << key.core.y() << ' ' << key.core.width() << ' ' << key.core.height() << ' '
        << coreRadius << ' ' << int(cached != nullptr);
  if (cached) {
    shadow = *cached;
    ++boxStats.hits;
  } else {
    ++boxStats.misses;
    diag::ScopedPerfProbe rasterPerf("paint.boxRasterize", effectPerf());
    shadow = rasterizeShadow(key);
    if (shadow.isNull())
      return;
    const auto kib = (shadow.sizeInBytes() + 1023) / 1024;
    if (kib <= shadows.maxCost())
      shadows.insert(key, new QImage(shadow), int(kib));
  }

  painter.save();
  diag::ScopedPerfProbe blitPerf("paint.boxBlit", effectPerf());
  painter.drawImage(imageBounds.topLeft(), shadow);
  painter.restore();
}

void paintBlockHoverGlow(QPainter& painter, const RenderTheme& theme, const QString& host, const QRectF& rect,
                         qreal phase) {
  const HoverEffect* he = hoverEffectFor(theme, host);
  if (!he) {
    return;
  }
  paintGlow(painter, rect, he->glowColor, he->glowBlur, phase);
}

} // namespace DecorationPainter
} // namespace muffin
