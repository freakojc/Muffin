#include "theme/CssComputedStyleEngine.h"
#include "theme/CssImageResources.h"
#include "theme/CssThemeMapper.h"
#include "theme/ThemeManager.h"
#include "render/CssPaintAnimation.h"
#include "render/LayoutBox.h"
#include "../TestUtils.h"

#include <QApplication>
#include <QBuffer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTemporaryDir>
#include <QUrl>

using namespace muffin;
namespace {
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write resource fixture");
}
ThemeElementStyle project(const CssThemeSheet& sheet) {
  CssElement element;
  element.tag = "div";
  element.id = "box";
  return CssThemeMapper::projectComputedStyle("#box", CssComputedStyleEngine(sheet).styleFor(element));
}
ThemeElementStyle project(const QString& css) {
  return project(CssThemeParser::parse("#box{" + css + "}", {}));
}
QImage png(QColor color) {
  QImage image(10, 6, QImage::Format_ARGB32);
  image.fill(color);
  return image;
}
QByteArray pngBytes(QColor color) {
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  require(png(color).save(&buffer, "PNG"), "encode raster");
  return bytes;
}

void testResourceScopesAndInvalidation() {
  QTemporaryDir dir;
  require(dir.isValid() && QDir(dir.path()).mkdir("base"), "temporary resource directory");
  const auto texture = dir.filePath("base/tile(1).png");
  write(texture, pngBytes(Qt::red));
  write(dir.filePath("base/base.css"), ":root{--texture:url('tile(1).png')}#box{background-image:var(--texture)}");
  const auto text = QStringLiteral("@import 'base/base.css';#box{background-repeat:round space}");
  const auto first = CssThemeParser::parse(text, dir.path(), dir.filePath("theme.css"));
  const auto a = project(first);
  require(a.paint.backgroundLayers.front().bitmap.pixelColor(0, 0) == QColor(Qt::red),
          "imported custom property URL keeps defining stylesheet base");
  const auto again = project(first);
  require(again.paint.backgroundLayers.front().resource == a.paint.backgroundLayers.front().resource,
          "style snapshots reuse decoded image source");
  require(CssThemeParser::localResourcePaths(text, dir.path(), true).contains(texture),
          "dependency manifest parses quoted parentheses");
  require(CssThemeParser::resourceUrls("url('tile\\28 1\\29.png')") == QStringList{"tile(1).png"},
          "CSS hexadecimal URL escapes decode once");
  write(texture, pngBytes(Qt::blue));
  const auto b = project(CssThemeParser::parse(text, dir.path(), dir.filePath("theme.css")));
  require(b.paint.backgroundLayers.front().bitmap.pixelColor(0, 0) == QColor(Qt::blue) &&
              a.paint.backgroundLayers.front().bitmap.pixelColor(0, 0) == QColor(Qt::red),
          "new resource snapshot refreshes bytes without mutating the old style");
  require(project(CssThemeParser::parse("#box{background-image:url('tile%281%29.png?cache=1')}", dir.filePath("base")))
                  .paint.backgroundLayers.front()
                  .bitmap.pixelColor(0, 0) == QColor(Qt::blue),
          "percent-encoded relative path and query resolve to the local file");
  const auto missing = dir.filePath("later.png");
  const auto missingSheet = CssThemeParser::parse("#box{background-image:url(later.png)}", dir.path());
  require(!project(missingSheet).paint.backgroundLayers.front().resource,
          "missing resource has an empty cached result");
  write(missing, pngBytes(Qt::green));
  require(project(CssThemeParser::parse("#box{background-image:url(later.png)}", dir.path()))
                  .paint.backgroundLayers.front()
                  .resource != nullptr,
          "reparse invalidates a negative resource cache");
  const QByteArray svg =
      "<svg xmlns='http://www.w3.org/2000/svg' width='10' height='6'><path fill='#123456' d='M0 0h10v6H0z'/></svg>";
  write(dir.filePath("base/vector.svg"), svg);
  const auto vector = project(CssThemeParser::parse("#box{background-image:url(vector.svg)}", dir.filePath("base")))
                          .paint.backgroundLayers.front()
                          .resource;
  require(vector && vector->intrinsicSize == QSizeF(10, 6) && !vector->svg.isEmpty(),
          "local SVG keeps intrinsic dimensions and vector bytes");
  const auto large = vector->raster({60, 36});
  require(large.pixelColor(30, 18) == QColor("#123456") && vector->raster({60, 36}).cacheKey() == large.cacheKey(),
          "SVG rasterizes at target size and caches the result");
  const auto encoded =
      "url('data:image/svg+xml," + QString::fromLatin1(QUrl::toPercentEncoding(QString::fromUtf8(svg))) + "')";
  require(project("background-image:" + encoded).paint.backgroundLayers.front().resource != nullptr,
          "percent-encoded SVG data URL loads");
  const auto localUrl = "url('" + QUrl::fromLocalFile(texture).toString() + "')";
  require(project("background-image:" + localUrl).paint.backgroundLayers.front().bitmap.pixelColor(0, 0) ==
              QColor(Qt::blue),
          "file URL loads the same local source");
  require(project("background-image:url(https://invalid.example/tile.png)").paint.backgroundLayers.front().resource ==
              nullptr,
          "remote image returns immediately without network loading");
  const auto animatedBase = project("background-image:" + encoded);
  const auto animatedTarget = project("background-image:" + localUrl);
  auto animated = animatedTarget;
  interpolateCssPaintProperty(animated, animatedBase, animatedTarget, "background-image", .25);
  require(animated.paint.backgroundLayers.front().resource == animatedBase.paint.backgroundLayers.front().resource,
          "image animation selects matching SVG/raster resource");

  // Production cache invalidation, including edits with identical file size
  // and timestamps. This checks the content manifest, not just the loader API.
  const auto css = dir.filePath("custom.css");
  write(css, "#box{background-image:url(base/vector.svg)}p{color:#123456}");
  ThemeManager manager(dir.path());
  require(manager.setTheme("custom"), "select local resource theme");
  const auto before = manager.currentDefinition();
  const auto previous = project(*before.sourceSheet);
  const auto path = dir.filePath("base/vector.svg");
  const auto stamp = QFileInfo(path).lastModified();
  auto updated = svg;
  updated.replace("#123456", "#654321");
  write(path, updated);
  QFile file(path);
  require(file.open(QIODevice::ReadWrite) && file.setFileTime(stamp, QFileDevice::FileModificationTime),
          "restore image timestamp");
  file.close();
  require(manager.setTheme("custom"), "reselect edited resource theme");
  const auto current = project(*manager.currentDefinition().sourceSheet);
  require(current.paint.backgroundLayers.front().resource->raster({10, 6}).pixelColor(4, 3) == QColor("#654321") &&
              previous.paint.backgroundLayers.front().resource->raster({10, 6}).pixelColor(4, 3) == QColor("#123456"),
          "content manifest invalidates same-size/same-time asset and retains previous snapshot");
}

void testBrowserPixels(const QString& path) {
  QFile fixture(path);
  require(fixture.open(QIODevice::ReadOnly), "read browser background oracle");
  const auto root = QJsonDocument::fromJson(fixture.readAll()).object();
  int cases = 0, failedCases = 0;
  for (const auto& entry : root.value("cases").toArray()) {
    const auto c = entry.toObject();
    const auto dpr = c.value("dpr").toDouble();
    const auto style = project(c.value("css").toString());
    ThemeElementBoxStyle used = style.box;
    QFont font;
    font.setPointSizeF(style.text.fontSizePx * .75);
    QImage image(qRound(120 * dpr), qRound(108 * dpr), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    image.fill(Qt::transparent);
    {
      QPainter painter(&image);
      paintLayoutBox(painter, LayoutBox::place("#box", style, used, {8, 8, 96, 84}, font));
    }
    int differing = 0, maximum = 0;
    const auto pixels = QByteArray::fromBase64(c.value("pixels").toString().toLatin1());
    const auto mask = QByteArray::fromBase64(c.value("mask").toString().toLatin1());
    int at = 0, sample = 0;
    for (int y = c.value("sampleStart").toInt(); y < image.height(); y += c.value("sampleStep").toInt())
      for (int x = c.value("sampleStart").toInt(); x < image.width(); x += c.value("sampleStep").toInt()) {
        require(at + 4 <= pixels.size(), "browser sample buffer has expected size");
        const auto actual = image.pixelColor(x, y);
        const int channels[]{actual.red(), actual.green(), actual.blue(), actual.alpha()};
        int difference = 0;
        for (int channel = 0; channel < 4; ++channel)
          difference = qMax(difference, qAbs(channels[channel] - int(static_cast<unsigned char>(pixels[at++]))));
        if (difference > 4 && mask[sample])
          ++differing;
        ++sample;
        maximum = qMax(maximum, difference);
      }
    require(at == pixels.size(), "all fixed browser samples were compared");
    if (differing) {
      const auto name = c.value("id").toString() + "-" + QString::number(dpr);
      image.save(QDir(QCoreApplication::applicationDirPath()).filePath("background-" + name + ".png"));
      std::fprintf(stderr, "background %s: %d differing samples, max %d\n", qPrintable(name), differing, maximum);
    }
    if (differing)
      ++failedCases;
    ++cases;
  }
  require(cases >= 100, "oracle covers tiling/blending and DPR 1/1.25/2");
  require(failedCases == 0, "background interiors/geometry agree with fixed Chromium samples");
}

void testCapabilities() {
  const auto fallback = project(
      "background-repeat:space;background-repeat:invalid;background-blend-mode:multiply;background-blend-mode:invalid");
  require(fallback.paint.backgroundLayers.front().repeatX == ThemeBackgroundLayer::Repeat::Space &&
              fallback.paint.backgroundLayers.front().blendMode == CssBlendMode::Multiply,
          "invalid repeat/blend declarations preserve the previous valid fallback");
  for (const auto& query : QStringList{"(background-repeat:space round)", "(background-repeat:round, space no-repeat)",
                                       "(background-blend-mode:multiply,hue)", "(background-image:url('tile.png'))",
                                       "(background-image:url('data:image/svg+xml,<svg/>'))"})
    require(CssThemeSheet::supportsMatches(query), "new background capability: " + query);
  for (const auto& query :
       QStringList{"(background-repeat:space extra)", "(background-blend-mode:unknown)", "(mix-blend-mode:hue)"})
    require(!CssThemeSheet::supportsMatches(query), "unsupported capability stays false: " + query);
}

void testPositionWideValuesAndInvalidVariables() {
  const auto checkOrigin = [](const ThemeElementStyle& style) {
    for (const auto& layer : style.paint.backgroundLayers)
      require(layer.positionX.px == 0 && layer.positionX.fraction == 0 && layer.positionY.px == 0 &&
                  layer.positionY.fraction == 0,
              "CSS-wide background-position resets both axes to the initial origin");
  };
  for (const auto& value : {"initial", "unset", "revert", "revert-layer", "var(--missing),0 0"})
    checkOrigin(project("background-position:30% 70%;background-position:" + QString::fromLatin1(value)));
  checkOrigin(project("--a:var(--b);--b:var(--a);background-position:var(--a)"));
  const auto fallback = project("background-position:var(--missing,20% 80%)");
  require(fallback.paint.backgroundLayers.front().positionX.fraction == .2 &&
              fallback.paint.backgroundLayers.front().positionY.fraction == .8,
          "valid variable fallback retains both explicit axes");
  const auto sheet = CssThemeParser::parse("div{background-position:25% 75%}#box{background-position:inherit}", {});
  CssElement parent;
  parent.tag = "div";
  CssElement child;
  child.tag = "div";
  child.id = "box";
  child.parent = &parent;
  const auto inherited = CssThemeMapper::projectComputedStyle("#box", CssComputedStyleEngine(sheet).styleFor(child));
  require(inherited.paint.backgroundLayers.front().positionX.fraction == .25 &&
              inherited.paint.backgroundLayers.front().positionY.fraction == .75,
          "background-position inherit preserves the parent's two axes");
  auto texture = project("background-position:var(--missing),0 0");
  QImage tile(7, 11, QImage::Format_ARGB32_Premultiplied);
  tile.fill(Qt::blue);
  for (int x = 0; x < tile.width(); ++x)
    tile.setPixelColor(x, 0, Qt::red);
  texture.paint.backgroundLayers.front().bitmap = tile;
  const auto render = [&](qreal height) {
    QImage image(64, 80, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::white);
    QPainter painter(&image);
    painter.setClipRect(image.rect());
    QFont font;
    font.setPointSizeF(12);
    paintLayoutBox(painter, LayoutBox::place("#box", texture, texture.box, {0, 0, 64, height}, font));
    return image;
  };
  require(render(1000000) == render(1000040),
          "estimated page height changes cannot shift an initial-origin repeating background");
}
} // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  require(argc > 1, "browser fixture argument");
  runTest("resource scopes and invalidation", testResourceScopesAndInvalidation);
  runTest("browser background pixels", [&] { testBrowserPixels(QString::fromLocal8Bit(argv[1])); });
  runTest("background feature queries", testCapabilities);
  runTest("background position wide values and invalid variables", testPositionWideValuesAndInvalidVariables);
  return 0;
}
