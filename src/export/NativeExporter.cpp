#include "export/NativeExporter.h"

#include "render/DocumentLayout.h"

#include <QImage>
#include <QPainter>
#include <QPrinter>
#include <cmath>

namespace muffin {

bool exportNativePdf(const QString& path, const MarkdownDocument& document, RenderTheme theme,
                     const QString& sourcePath, const PdfExportOptions& options,
                     mermaid::editor::MermaidRenderCache* cache) {
  QPrinter printer;
  printer.setResolution(96);
  printer.setOutputFormat(QPrinter::PdfFormat);
  printer.setOutputFileName(path);
  if (!printer.setPageLayout(options.pageLayout))
    return false;
  const QRectF page = printer.pageRect(QPrinter::DevicePixel);
  if (page.isEmpty())
    return false;
  DocumentLayout layout;
  layout.setMermaidRenderCache(cache);
  layout.setMermaidSyncMode(true);
  layout.rebuild(document, theme, page.width(), sourcePath);
  QPainter painter(&printer);
  if (!painter.isActive())
    return false;
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
  qreal top = 0;
  bool pageHasContent = false;
  const auto nextPage = [&] {
    if (!printer.newPage())
      return false;
    pageHasContent = false;
    return true;
  };
  for (const auto* block : layout.promotedBlocks()) {
    const auto rect = block->rect();
    if ((rect.top() >= top + page.height()) ||
        (options.breakOnHeading && block->headingLevel() == 1 && pageHasContent)) {
      if (!nextPage())
        return false;
      top = rect.top();
    }
    for (;;) {
      painter.save();
      // QPrinter's default origin is already the printable area's top left.
      // Translating by page.topLeft() here would apply the margins twice.
      painter.setClipRect(QRectF(0, 0, page.width(), page.height()));
      if (!pageHasContent)
        painter.fillRect(QRectF(0, 0, page.width(), page.height()), theme.backgroundColor());
      block->paint(painter, theme, top);
      painter.restore();
      pageHasContent = true;
      if (rect.bottom() <= top + page.height())
        break;
      if (!nextPage())
        return false;
      top += page.height();
    }
  }
  return painter.end();
}

bool exportNativeImage(const QString& path, const MarkdownDocument& document, RenderTheme theme,
                       const QString& sourcePath, const ImageExportOptions& options,
                       mermaid::editor::MermaidRenderCache* cache) {
  DocumentLayout layout;
  layout.setMermaidRenderCache(cache);
  layout.setMermaidSyncMode(true);
  layout.rebuild(document, theme, options.width, sourcePath);
  const qreal height = qMax(1.0, std::ceil(layout.totalHeight()));
  const qreal pixelWidth = options.width * options.scale;
  const qreal pixelHeight = height * options.scale;
  if (!std::isfinite(pixelHeight) || pixelWidth < 1 || pixelHeight > 32767 || pixelWidth * pixelHeight > 64000000)
    return false;
  QImage image(QSize(int(pixelWidth), int(pixelHeight)), QImage::Format_ARGB32_Premultiplied);
  if (image.isNull())
    return false;
  image.fill(theme.backgroundColor());
  QPainter painter(&image);
  painter.setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing);
  painter.scale(options.scale, options.scale);
  for (const auto* block : layout.promotedBlocks())
    block->paint(painter, theme, 0);
  painter.end();
  return image.save(path, "PNG");
}

} // namespace muffin
