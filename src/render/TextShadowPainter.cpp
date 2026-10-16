#include "render/TextShadowPainter.h"

#include <QCache>
#include <QDataStream>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <atomic>
#include <cmath>

#include "diagnostics/ScopedPerfProbe.h"
#include "render/Blur.h"
#include "render/Filter.h"

namespace muffin {
namespace {
Q_LOGGING_CATEGORY(shadowPerf, "muffin.perf.paint", QtWarningMsg)
constexpr int cacheBudget = 16 * 1024 * 1024;
constexpr int maxEntries = 512;
constexpr int blurPasses = 3;
struct Key {
  quint64 owner;
  QByteArray values;
  bool operator==(const Key&) const = default;
  friend size_t qHash(const Key& key, size_t seed = 0) { return qHashMulti(seed, key.owner, key.values); }
};
struct Store {
  QCache<Key, QImage> images{cacheBudget};
};
thread_local std::shared_ptr<Store> store = std::make_shared<Store>();
thread_local TextShadowRasterStats stats;

// Merge intersecting halo rectangles before drawing: overlapping shadows from
// one source are one convolution, not independent translucent layers. Disjoint
// visual lines remain sparse even when far apart in the same paragraph.
QVector<QRect> islands(const QRegion& region, int halo) {
  QVector<QRect> result;
  for (const auto& rect : region) {
    auto area = rect.adjusted(-halo, -halo, halo, halo);
    for (qsizetype i = 0; i < result.size();) {
      if (area.intersects(result[i])) {
        area = area.united(result.takeAt(i));
        i = 0;
      } else
        ++i;
    }
    result.append(area);
  }
  return result;
}

// QTextLayout::draw(selections) clips the unselected text to boundingRect().
// That rectangle can cut the final underline's antialiasing coverage, and its
// transformed edge differs between a sparse surface and a full canvas. For
// disjoint paint-only overrides, resolve formats before drawing instead. No
// font/shaping property changes, and the original line geometry is retained.
std::unique_ptr<TextLayout> shadowTextWithFormats(const TextLayout& source,
                                                 const QList<TextLayout::FormatRange>& overrides) {
  if (overrides.isEmpty()) return {};
  for (int i = 0; i < overrides.size(); ++i) {
    const auto& range = overrides[i];
    for (const auto property : range.format.properties().keys())
      if (property != QTextFormat::ForegroundBrush && property != QTextFormat::FontUnderline &&
          property != QTextFormat::FontOverline && property != QTextFormat::FontStrikeOut &&
          property != QTextFormat::TextUnderlineStyle && property != QTextFormat::TextUnderlineColor &&
          property != QTextFormat::TextOutline)
        return {};
    for (int j = 0; j < i; ++j)
      if (range.start < overrides[j].start + overrides[j].length &&
          overrides[j].start < range.start + range.length)
        return {};
  }
  auto text = std::make_unique<TextLayout>(source.text(), source.font(), source.backend());
  text->setTextOption(source.textOption());
  auto formats = source.formats();
  formats.append(overrides);
  text->setFormats(formats);
  text->beginLayout();
  for (int i = 0; i < source.lineCount(); ++i) {
    auto line = text->createLine();
    const auto original = source.lineAt(i);
    if (!line.isValid()) {
      text->endLayout();
      return {};
    }
    line.setLineWidth(original.width());
    line.setPosition(original.position());
    if (line.textStart() != original.textStart() || line.textLength() != original.textLength()) {
      text->endLayout();
      return {};
    }
  }
  text->endLayout();
  return text;
}
}  // namespace

struct TextShadowPainter::Owner {
  std::weak_ptr<Store> cache = store;
  quint64 id = [] {
    static std::atomic<quint64> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }();
  ~Owner() {
    if (const auto current = cache.lock())
      for (const auto& key : current->images.keys())
        if (key.owner == id) current->images.remove(key);
  }
};

TextShadowRasterStats textShadowRasterStats() {
  auto result = stats;
  result.cacheBytes = store->images.totalCost();
  result.cacheEntries = store->images.size();
  return result;
}
void resetTextShadowRasterStats() { stats = {}; }

void TextShadowPainter::paint(QPainter& painter, const TextLayout& text, QPointF origin, const TextShadow& shadow,
                              qreal zoom, const QVector<QRectF>& rects,
                              const QList<TextLayout::FormatRange>& formats) const {
  diag::ScopedPerfProbe perf("paint.textShadow", shadowPerf());
  if (!shadow.present || !shadow.color.isValid() || shadow.color.alpha() == 0 || !text.lineCount()) return;
  const qreal dpr = painter.device()->devicePixelRatioF();
  const qreal blur = qMax<qreal>(0, shadow.blur * zoom * dpr);
  const int radius = blur > 0 ? qMax(1, qRound(blur)) : 0;
  // Three box passes have radius*3 support. Leave transparent samples outside
  // that support so edge clamping cannot stretch glow at a cropped boundary.
  const int halo = radius * blurPasses + 2;
  const QPointF offset = shadow.offset * zoom;
  // Round one common anchor before adding integer raster coordinates. Qt's
  // nearest-image rounding at half pixels differs across zero; rounding each
  // island separately would shift a cropped shadow by one device pixel.
  const auto deviceTransform = painter.deviceTransform();
  bool invertible = false;
  const auto inverse = deviceTransform.inverted(&invertible);
  QPointF anchor = origin + offset;
  if (invertible && deviceTransform.m12() == 0 && deviceTransform.m21() == 0 &&
      qFuzzyCompare(deviceTransform.m11(), dpr) && qFuzzyCompare(deviceTransform.m22(), dpr)) {
    auto deviceAnchor = deviceTransform.map(anchor);
    deviceAnchor = {std::floor(deviceAnchor.x() + .5), std::floor(deviceAnchor.y() + .5)};
    anchor = inverse.map(deviceAnchor);
  }
  QPainterPath sourceClip;
  QRegion source;
  const auto physical = [dpr](QRectF rect) { return QRectF(rect.topLeft() * dpr, rect.size() * dpr).toAlignedRect(); };
  if (!rects.isEmpty()) {
    for (auto rect : rects) {
      rect.adjust(-1, 0, 1,
                  0);  // Retain the existing inline glyph/decoration clip.
      sourceClip.addRect(rect);
      source += physical(rect);
    }
  } else {
    // Host/composited shadows draw the complete shaped stream, including font
    // overhangs, outlines and decorations, rather than clipping to advances.
    qreal overhang = textFontPixelSize(text.font()) + 2;
    const auto includeOverhang = [&](const QList<TextLayout::FormatRange>& ranges) {
      for (const auto& format : ranges)
        overhang = qMax(overhang, qMax(format.format.fontPointSize() * 96 / 72,
                                       format.format.doubleProperty(QTextFormat::FontPixelSize)) +
                                      format.format.textOutline().widthF() + 2);
    };
    includeOverhang(text.formats());
    includeOverhang(formats);
    for (int i = 0; i < text.lineCount(); ++i)
      source += physical(text.lineAt(i).naturalTextRect().adjusted(-overhang, -overhang, overhang, overhang));
  }
  auto visible = cssPaintDeviceBounds(painter);
  if (painter.hasClipping()) visible = visible.intersected(painter.clipBoundingRect());
  const auto demand = physical(visible.translated(-origin - offset));
  QByteArray inputs;
  {
    QDataStream stream(&inputs, QIODevice::WriteOnly);
    stream << dpr << radius << shadow.color << rects << quint64(formats.size());
    for (const auto& format : formats) stream << format.start << format.length << format.format;
  }
  if (!owner_) owner_ = std::make_shared<Owner>();
  std::unique_ptr<TextLayout> formattedText;
  bool preparedFormats = false;
  for (auto area : islands(source, halo)) {
    ++stats.requests;
    if (!area.intersects(demand)) {
      ++stats.culled;
      continue;
    }
    // Large host shadows retain only the demanded region plus blur support;
    // small word-sized islands remain independent of viewport/scroll position.
    if (qint64(area.width()) * area.height() > 1024 * 1024)
      area = area.intersected(demand.adjusted(-halo, -halo, halo, halo));
    QByteArray values = inputs;
    QDataStream stream(&values, QIODevice::Append);
    stream << area;
    const Key key{owner_->id, values};
    QImage raster;
    if (const auto* cached = store->images.object(key)) {
      raster = *cached;
      ++stats.hits;
    } else {
      ++stats.misses;
      const quint64 pixels = quint64(area.width()) * area.height();
      stats.rasterizedPixels += pixels;
      stats.largestSurfacePixels = qMax(stats.largestSurfacePixels, pixels);
      const QPointF local = QPointF(area.topLeft()) / dpr;
      if (!preparedFormats) {
        formattedText = shadowTextWithFormats(text, formats);
        preparedFormats = true;
      }
      QImage mask;
      {
        diag::ScopedPerfProbe maskPerf("paint.shadowMask", shadowPerf());
        mask = QImage(area.size(), QImage::Format_ARGB32_Premultiplied);
        if (mask.isNull()) continue;
        mask.setDevicePixelRatio(dpr);
        mask.fill(Qt::transparent);
        QPainter target(&mask);
        target.setRenderHint(QPainter::Antialiasing);
        target.setRenderHint(QPainter::TextAntialiasing);
        target.translate(-local);
        // A draw clip also changes QTextLayout's selection/exclusion paths.
        // Keep those paths independent of the sparse surface when applying
        // foreground overrides; the paint device still clips raster work.
        // Static text can use the bounds to skip unrelated visual lines.
        if (formattedText)
          formattedText->draw(&target, {}, {}, QRectF(local, QSizeF(area.size()) / dpr));
        else
          text.draw(&target, {}, formats, formats.isEmpty() ? QRectF(local, QSizeF(area.size()) / dpr) : QRectF());
      }
      if (!rects.isEmpty()) {
        // Apply coverage once to the composed text, in device coordinates.
        // CoreText/Qt can change clip coverage when the fractional text backend
        // temporarily scales the painter by 1/64. A separate coverage image
        // keeps clipping independent of that glyph/outline paint transform.
        raster = QImage(area.size(), QImage::Format_ARGB32_Premultiplied);
        if (raster.isNull()) continue;
        raster.fill(Qt::transparent);
        {
          QPainter coverage(&raster);
          coverage.setRenderHint(QPainter::Antialiasing);
          const auto deviceClip = QTransform::fromScale(dpr, dpr).map(sourceClip).translated(-area.topLeft());
          coverage.fillPath(deviceClip, Qt::white);
        }
        {
          QPainter coverage(&mask);
          coverage.setCompositionMode(QPainter::CompositionMode_DestinationIn);
          raster.setDevicePixelRatio(dpr);
          coverage.drawImage(QPointF(), raster);
        }
      }
      const bool traceRaster = !formats.isEmpty() && qEnvironmentVariableIsSet("MUFFIN_SHADOW_RASTER_TRACE");
      if (traceRaster) {
        const auto name = QString("resource-pixels-sparse-%1-%2").arg(area.x()).arg(area.y());
        mask.save(name + "-mask.png");
        qWarning() << "Sparse source area" << area;
      }
      {
        diag::ScopedPerfProbe tintPerf("paint.shadowTint", shadowPerf());
        raster = QImage(area.size(), QImage::Format_ARGB32_Premultiplied);
        if (raster.isNull()) continue;
        raster.setDevicePixelRatio(dpr);
        raster.fill(Qt::transparent);
        QPainter target(&raster);
        target.fillRect(QRectF({}, QSizeF(area.size()) / dpr), shadow.color);
        target.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        target.drawImage(QPointF(), mask);
      }
      if (traceRaster)
        raster.save(QString("resource-pixels-sparse-%1-%2-tint.png").arg(area.x()).arg(area.y()));
      if (radius) {
        diag::ScopedPerfProbe blurPerf("paint.shadowBlur", shadowPerf());
        boxBlur(raster, radius, blurPasses);
      }
      if (raster.sizeInBytes() <= cacheBudget && inputs.size() <= 65536) {
        if (store->images.size() >= maxEntries) store->images.remove(store->images.keys().front());
        store->images.insert(key, new QImage(raster), int(raster.sizeInBytes()));
      }
    }
    diag::ScopedPerfProbe blitPerf("paint.shadowBlit", shadowPerf());
    painter.drawImage(anchor + QPointF(area.topLeft()) / dpr, raster);
  }
}
}  // namespace muffin
