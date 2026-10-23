#pragma once

#include <QCache>
#include <QImage>
#include <QSizeF>
#include <QPointF>
#include <QString>
#include <memory>

namespace muffin {
namespace diag {
struct MemoryAccounting;
}
struct CssSvgRasterCache;

// Decoded immutable source. SVG stays vector data until the painter knows the
// target tile size/DPR; derived rasters are bounded and shared by style copies.
class CssImageResource {
public:
  QImage bitmap;
  QByteArray svg;
  QSizeF intrinsicSize;
  bool intrinsicWidth = true, intrinsicHeight = true;
  qreal intrinsicRatio = 0;
  QSizeF svgViewBoxSize;
  QPointF svgAlignment{.5, .5};
  bool svgPreserveAspect = true, svgSlice = false;
  QImage raster(const QSize& pixels) const;

private:
  friend class CssImageResources;
  std::shared_ptr<CssSvgRasterCache> rasters_;
  QByteArray rasterKey_;
};

// Snapshot-owned: cache hits never stat/decode the same file during layout.
// A reparsed theme gets a new cache, including formerly missing resources.
// ThemeManager's content manifest is the reload/invalidation boundary.
class CssImageResources {
  friend struct diag::MemoryAccounting;

public:
  CssImageResources();
  std::shared_ptr<const CssImageResource> load(const QString& target) const;
  static bool supportsUrl(const QString& target);
  static QString localPath(const QString& target, const QString& baseDir = {});

private:
  mutable QCache<QString, std::shared_ptr<const CssImageResource>> cache_{32 * 1024 * 1024};
  std::shared_ptr<CssSvgRasterCache> rasters_;
};

} // namespace muffin
