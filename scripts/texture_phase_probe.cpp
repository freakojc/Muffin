// Isolated real-theme raster diagnostic. No foreground/input timing claims.
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QScrollBar>
#include <QTimer>
#include <cstdio>

#include "document/DocumentSession.h"
#include "editor/EditorView.h"
#include "render/CssBackgroundPainter.h"
#include "render/LayoutBox.h"
#include "theme/CssThemeMapper.h"
#include "theme/RenderTheme.h"

using namespace muffin;
namespace {
QString fingerprint(const QImage& image) {
  return QString::fromLatin1(
      QCryptographicHash::hash(QByteArrayView(reinterpret_cast<const char*>(image.constBits()), image.sizeInBytes()),
                               QCryptographicHash::Sha256)
          .toHex());
}
QImage raster(const ThemeElementStyle& style, QFont font, qreal height, QPoint offset, bool split) {
  QImage image(1100, 820, QImage::Format_ARGB32_Premultiplied);
  image.fill(QColor("#1a1915"));
  QPainter painter(&image);
  painter.setRenderHint(QPainter::Antialiasing);
  painter.translate(offset);
  const auto box = LayoutBox::place("#write", style, style.box, {0, 20, 1086, height}, font);
  for (int part = 0; part < (split ? 2 : 1); ++part) {
    painter.save();
    painter.setClipRect(split ? QRect(0, part * 400, 1086, 400) : QRect(0, 0, 1086, 800));
    paintCssBackgrounds(painter, box, {}, style.paint, 1);
    painter.restore();
  }
  return image;
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  if (argc != 3)
    return 2;
  QFile css(QString::fromLocal8Bit(argv[1]));
  if (!css.open(QIODevice::ReadOnly))
    return 2;
  auto theme = RenderTheme::fromDefinition(
      CssThemeMapper::fromCss(QString::fromUtf8(css.readAll()), "vlook-probe", QFileInfo(css).absolutePath()));
  theme.updateForViewport(1086, 741);
  DocumentSession session;
  session.setMarkdownText("# Theme acceptance\n\nPlain **bold** text.\n", false);
  theme.resolveDocumentCounters(session.document());
  const auto* style = theme.elementStyleForNode(session.document().root(), "#write");
  if (!style || style->paint.backgroundLayers.empty())
    return 3;
  QJsonArray layers;
  for (const auto& layer : style->paint.backgroundLayers)
    layers.append(QJsonObject{{"image_width", layer.bitmap.width()},
                              {"image_height", layer.bitmap.height()},
                              {"position_x_px", layer.positionX.px},
                              {"position_x_fraction", layer.positionX.fraction},
                              {"position_y_px", layer.positionY.px},
                              {"position_y_fraction", layer.positionY.fraction}});
  const auto directory = QString::fromLocal8Bit(argv[2]);
  if (!QDir().mkpath(directory))
    return 2;
  QJsonArray rows;
  for (const auto offset : {QPoint(0, 0), QPoint(0, 30)}) {
    const auto reference = raster(*style, theme.paragraphFont(), 1000, offset, false);
    for (const qreal height : {1000., 32767., 65535., 1000000., 1000020., 1000040., 10000000.}) {
      for (bool split : {false, true}) {
        const auto image = raster(*style, theme.paragraphFont(), height, offset, split);
        const auto name = QString("%1-%2-%3.png").arg(height).arg(offset.y()).arg(split);
        if (!image.save(QDir(directory).filePath(name)))
          return 3;
        rows.append(QJsonObject{{"height", height},
                                {"offset_y", offset.y()},
                                {"split", split},
                                {"sha256", fingerprint(image)},
                                {"same_as_reference", image == reference}});
      }
    }
  }
  QJsonArray editorRows;
  QString text = "# Theme acceptance\n\nPlain **bold**, *italic*, `code`, [Muffin](https://example.com).\n\n";
  const auto paragraph =
      QString("Plain text with **bold**, *italic*, ~~strike~~, `code`, and [Muffin](https://example.com). ")
          .repeated(12) +
      "\n\n";
  while (text.size() < 10 * 1024 * 1024)
    text += paragraph;
  DocumentSession large;
  large.setMarkdownText(text, false);
  EditorView view;
  // Exercise native-QPA lazy layout without showing or activating a desktop
  // window. QWidget::grab performs the raster; this is not GUI timing data.
  view.setAttribute(Qt::WA_DontShowOnScreen);
  view.resize(1086, 741);
  view.setDocument(large.document());
  view.show();
  QImage reference;
  for (int cycle = 0; cycle < 8; ++cycle) {
    view.setTheme(RenderTheme::github());
    view.setTheme(theme);
    view.verticalScrollBar()->setValue(0);
    QEvent leave(QEvent::Leave);
    QCoreApplication::sendEvent(&view, &leave);
    QEventLoop settle;
    QTimer::singleShot(700, &settle, &QEventLoop::quit);
    settle.exec();
    const auto image = view.viewport()->grab().toImage();
    if (reference.isNull())
      reference = image;
    if (!image.save(QDir(directory).filePath(QString("editor-cycle-%1.png").arg(cycle))))
      return 3;
    const auto first = view.nodeRect(large.document().root().children().front()->id());
    editorRows.append(QJsonObject{{"cycle", cycle},
                                  {"same_as_first", image == reference},
                                  {"total_height", view.layoutTotalHeight()},
                                  {"scroll_y", view.verticalScrollBar()->value()},
                                  {"first_block", QJsonArray{first.x(), first.y(), first.width(), first.height()}}});
  }
  const QJsonObject result{{"layers", layers},
                           {"cases", rows},
                           {"editor_cycles", editorRows},
                           {"qpa", QGuiApplication::platformName()},
                           {"method", "Real VLOOK #write style; isolated QImage raster, no GUI latency measurement."}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
}
