#include "theme/CssImageResources.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QPainter>
#include <QSvgRenderer>
#include <QUrl>
#include <QXmlStreamReader>

#include <cmath>
#include <climits>

namespace muffin {
struct CssSvgRasterCache { QCache<QByteArray, QImage> entries{16 * 1024 * 1024}; };
CssImageResources::CssImageResources() : rasters_(std::make_shared<CssSvgRasterCache>()) {}
namespace {
constexpr qint64 maxSourceBytes = 32 * 1024 * 1024;
constexpr qint64 maxPixels = 16 * 1024 * 1024;
bool usableSize(QSizeF size) {
  return std::isfinite(size.width()) && std::isfinite(size.height()) && size.width() > 0 && size.height() > 0 &&
         size.width() * size.height() <= maxPixels;
}
}

QString CssImageResources::localPath(const QString& target, const QString& baseDir) {
  if (target.isEmpty() || target.startsWith('#') || target.startsWith("//") || target.startsWith("data:", Qt::CaseInsensitive)) return {};
  if (target.startsWith(":/")) return QDir::cleanPath(target);
  // A drive path is not a URL scheme. file: URLs retain percent-encoded paths.
  const QUrl url(target);
  if (url.isLocalFile()) return QDir::cleanPath(url.toLocalFile());
  if (QDir::isAbsolutePath(target)) return QDir::cleanPath(target);
  if (!url.scheme().isEmpty()) return {};
  return QDir::cleanPath(QDir(baseDir).filePath(url.path(QUrl::FullyDecoded)));
}

bool CssImageResources::supportsUrl(const QString& target) {
  if (!target.startsWith("data:", Qt::CaseInsensitive)) return !target.isEmpty() && !target.startsWith('#');
  const auto comma = target.indexOf(',');
  if (comma < 0) return false;
  const auto mime = target.mid(5, comma - 5).section(';', 0, 0).toLower();
  return mime == "image/png" || mime == "image/jpeg" || mime == "image/bmp" || mime == "image/gif" ||
         mime == "image/webp" || mime == "image/svg+xml";
}

std::shared_ptr<const CssImageResource> CssImageResources::load(const QString& target) const {
  if (const auto cached = cache_.object(target)) return *cached;
  QByteArray bytes;
  if (supportsUrl(target) && target.startsWith("data:", Qt::CaseInsensitive)) {
    const auto comma = target.indexOf(',');
    const auto header = target.left(comma);
    if (target.size() - comma <= maxSourceBytes * 4 / 3 + 4)
      bytes = header.contains(";base64", Qt::CaseInsensitive)
          ? QByteArray::fromBase64(target.mid(comma + 1).toLatin1(), QByteArray::AbortOnBase64DecodingErrors)
          : QByteArray::fromPercentEncoding(target.mid(comma + 1).toUtf8());
  } else {
    const auto path = localPath(target);
    if (!path.isEmpty()) {
      QFile file(path);
      if (file.open(QIODevice::ReadOnly) && file.size() <= maxSourceBytes) bytes = file.readAll();
    }
  }
  std::shared_ptr<CssImageResource> resource;
  if (!bytes.isEmpty()) {
    QBuffer buffer(&bytes); buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);
    const auto format = reader.format().toLower();
    if (format == "svg" || format == "svgz" || bytes.left(1024).contains("<svg")) {
      QSvgRenderer renderer(bytes);
      renderer.setAnimationEnabled(false);
      if (renderer.isValid()) {
        auto size = QSizeF(renderer.defaultSize());
        if (size.isEmpty()) size = renderer.viewBoxF().size();
        if (usableSize(size)) {
          resource = std::make_shared<CssImageResource>();
          resource->svg = bytes; resource->intrinsicSize = size;
          resource->rasters_ = rasters_;
          resource->rasterKey_ = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
          resource->svgViewBoxSize = renderer.viewBoxF().size();
          QXmlStreamReader xml(bytes);
          if (xml.readNextStartElement()) {
            const auto aspect = xml.attributes().value("preserveAspectRatio").toString();
            const auto width = xml.attributes().value("width").toString(), height = xml.attributes().value("height").toString();
            resource->intrinsicWidth = !width.isEmpty() && !width.contains('%');
            resource->intrinsicHeight = !height.isEmpty() && !height.contains('%');
            const bool viewBox = xml.attributes().hasAttribute("viewBox");
            resource->svgPreserveAspect = viewBox && aspect != "none";
            resource->svgSlice = aspect.contains("slice");
            resource->svgAlignment.setX(aspect.contains("xMin") ? 0 : aspect.contains("xMax") ? 1 : .5);
            resource->svgAlignment.setY(aspect.contains("YMin") ? 0 : aspect.contains("YMax") ? 1 : .5);
            if (resource->intrinsicWidth && resource->intrinsicHeight) resource->intrinsicRatio = size.width() / size.height();
            else if (viewBox && !resource->svgViewBoxSize.isEmpty())
              resource->intrinsicRatio = resource->svgViewBoxSize.width() / resource->svgViewBoxSize.height();
          }
        }
      }
    } else if (usableSize(reader.size())) {
      const auto image = reader.read();
      if (!image.isNull()) {
        resource = std::make_shared<CssImageResource>();
        resource->bitmap = image; resource->intrinsicSize = image.size();
        resource->intrinsicRatio = double(image.width()) / image.height();
      }
    }
  }
  const qint64 cost = 1 + target.size() * 2 + (resource ? resource->bitmap.sizeInBytes() + resource->svg.size() : 0);
  std::shared_ptr<const CssImageResource> result = resource;
  if (cache_.size() >= 64) cache_.clear();
  cache_.insert(target, new std::shared_ptr<const CssImageResource>(result), int(qMin(cost, qint64(INT_MAX))));
  return result;
}

QImage CssImageResource::raster(const QSize& pixels) const {
  if (svg.isEmpty()) return bitmap;
  if (!usableSize(pixels)) return {};
  const auto key = rasterKey_ + QByteArray::number(pixels.width()) + 'x' + QByteArray::number(pixels.height());
  if (rasters_) if (const auto cached = rasters_->entries.object(key)) return *cached;
  QImage image(pixels, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
  QSvgRenderer renderer(svg);
  renderer.setAnimationEnabled(false);
  QPainter painter(&image);
  QRectF target(QPointF(), pixels);
  if (svgPreserveAspect && !svgViewBoxSize.isEmpty()) {
    const auto x = pixels.width() / svgViewBoxSize.width(), y = pixels.height() / svgViewBoxSize.height();
    const auto fitted = svgViewBoxSize * (svgSlice ? qMax(x, y) : qMin(x, y));
    target = QRectF(QPointF((pixels.width() - fitted.width()) * svgAlignment.x(),
                           (pixels.height() - fitted.height()) * svgAlignment.y()), fitted);
  }
  renderer.render(&painter, target);
  painter.end();
  if (rasters_) rasters_->entries.insert(key, new QImage(image), int(image.sizeInBytes()));
  return image;
}
}  // namespace muffin
