#include <QApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QFontDatabase>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRawFont>
#include <QTemporaryDir>
#include <QtEndian>

#include "../TestUtils.h"
#include "document/DocumentSession.h"
#include "render/DocumentLayout.h"
#include "render/InlineLayout.h"
#include "theme/CssThemeMapper.h"
#include "theme/FontRendering.h"
#include "theme/ThemeFontDecoder.h"
#include "theme/ThemeManager.h"
using namespace muffin;
namespace {
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size(), "write font test CSS");
}
QString faceCss(const QJsonArray& faces) {
  QString css;
  for (const auto& value : faces) {
    const auto f = value.toObject();
    css += QString("@font-face{font-family:'%1';src:url('%2/%3');font-weight:%4;font-style:%5}")
               .arg(f["family"].toString(), QStringLiteral(MUFFIN_SOURCE_DIR), f["file"].toString())
               .arg(f["weight"].toInt())
               .arg(f["style"].toString());
  }
  return css;
}
void browserFonts(const QJsonObject& reference) {
  QTemporaryDir dir;
  require(dir.isValid(), "font fixture directory");
  for (const auto& value : reference["cases"].toArray()) {
    const auto c = value.toObject(), expected = c["expected"].toObject();
    const auto id = c["id"].toString();
    const auto tag = c["tag"].toString("p");
    const auto css = faceCss(c["faces"].toArray()) + "#write{padding:0;margin:0;" + c["parent"].toString() + "}" + tag +
                     "{margin:0;line-height:1.5;" + c["css"].toString() + "}";
    const auto path = dir.filePath("theme.css");
    write(path, css.toUtf8());
    const auto theme = RenderTheme::fromDefinition(ThemeDefinition::fromCss(path, id));
    DocumentSession session;
    const auto text = c["text"].toString();
    session.setMarkdownText(c["markdown"].toString((tag == "h1" ? "# " : "") + text), false);
    DocumentLayout layout;
    layout.rebuild(session.document(), theme, c["width"].toDouble());
    const auto& node = *session.document().root().children().front();
    const auto* block = layout.block(node.id());
    const auto* inlines = block->inlineLayout();
    require(inlines && inlines->displayText() == text, id + " font test text");
    const auto font = block->cssBoxGeometry().font;
    require(int(font.weight()) == qRound(expected["fontWeight"].toString().toDouble()),
            id + " inherited/descriptor font weight");
    require(font.italic() == (expected["fontStyle"].toString() == "italic"), id + " italic face selection");
    const auto characters = expected["characters"].toArray();
    for (int i = 0; i < characters.size(); ++i) {
      const auto e = characters[i].toObject();
      const auto caret = inlines->cursorRect(i);
      require(qAbs(caret.x() - e["x"].toDouble()) < .7,
              id + QString(" character %1 x actual=%2 expected=%3").arg(i).arg(caret.x()).arg(e["x"].toDouble()));
    }
    require(qAbs(inlines->height() - expected["height"].toDouble()) < .7, id + " browser line count/height");
  }
}
void isolationAndReload() {
  QTemporaryDir dir;
  const auto path = dir.filePath("theme.css");
  const auto rule = [](const QString& filename) {
    return QString(
               "@font-face{font-family:'Shared "
               "Alias';src:url('%1/tests/fixtures/theme/fonts/%2')}p{font-family:'Shared Alias';font-size:20px}")
        .arg(MUFFIN_SOURCE_DIR, filename)
        .toUtf8();
  };
  write(path, rule("latex-regular.woff2"));
  const auto first = ThemeDefinition::fromCss(path, "first");
  const auto old = RenderTheme::fromDefinition(first).paragraphFont();
  write(path, rule("abyss.woff2"));
  const auto second = ThemeDefinition::fromCss(path, "second");
  require(first.fontAliases.value("shared alias") != second.fontAliases.value("shared alias"),
          "same CSS alias has independent content identities");
  require(RenderTheme::fromDefinition(first).paragraphFont() == old,
          "reloading leaves old document font snapshots unchanged");
  write(path, rule("latex-regular.woff2"));
  const auto again = ThemeDefinition::fromCss(path, "again");
  require(again.fontAliases == first.fontAliases, "same font contents reuse the private family");
  // Existing but corrupt first source, missing local name, then a usable WOFF2.
  write(dir.filePath("corrupt.ttf"), "not a font");
  auto css = rule("latex-regular.woff2");
  css.replace("src:url(", "src:local('Missing Muffin Local Font'),url('corrupt.ttf'),url(");
  write(path, css);
  require(!ThemeDefinition::fromCss(path, "fallback").fontAliases.isEmpty(),
          "ordered source fallback skips missing and corrupt candidates");
}
void preparedFontChecksumsAndIdentity() {
  QFile file(QStringLiteral(MUFFIN_SOURCE_DIR "/tests/fixtures/theme/fonts/latex-regular.woff2"));
  require(file.open(QIODevice::ReadOnly), "open font checksum fixture");
  const auto source = file.readAll();
  const auto prepared = prepareThemeFont(source);
  require(!prepared.native.isEmpty() &&
              prepared.digest == QCryptographicHash::hash(prepared.native, QCryptographicHash::Sha256),
          "selected font content is addressed by its decoded bytes");
  require(prepareThemeFont(QByteArray("not a font")).native.isEmpty(), "invalid font candidates remain rejected");
  const auto sum = [](const QByteArray& bytes) {
    quint32 result = 0;
    for (qsizetype i = 0; i < bytes.size(); i += 4) {
      quint32 word = 0;
      for (int j = 0; j < 4; ++j) word = (word << 8) | (i + j < bytes.size() ? quint8(bytes[i + j]) : 0);
      result += word;
    }
    return result;
  };
  const auto get16 = [](const QByteArray& b, qsizetype i) { return qFromBigEndian<quint16>(b.constData() + i); };
  const auto get32 = [](const QByteArray& b, qsizetype i) { return qFromBigEndian<quint32>(b.constData() + i); };
  auto withoutDescriptors = prepared.native;
  for (int i = 0; i < get16(withoutDescriptors, 4); ++i) {
    const auto at = 12 + 16 * i;
    if (get32(withoutDescriptors, at) == 0x4f532f32) qToBigEndian<quint32>(0x4e4f5332, withoutDescriptors.data() + at);
  }
  require(!prepareThemeFont(withoutDescriptors).native.isEmpty() &&
              prepareThemeFont(withoutDescriptors, 400, false).native.isEmpty(),
          "fonts without OS/2 remain usable when no CSS descriptors are requested");
  const auto verify = [&](const QByteArray& native) {
    require(!native.isEmpty() && sum(native) == 0xB1B0AFBA, "independent byte-wise whole-font checksum remains exact");
    for (int i = 0; i < get16(native, 4); ++i) {
      const auto at = 12 + 16 * i;
      auto table = native.mid(get32(native, at + 8), get32(native, at + 12));
      if (get32(native, at) == 0x68656164) qToBigEndian<quint32>(0, table.data() + 8);
      require(sum(table) == get32(native, at + 4), "independent table checksum matches padded big-endian sum");
    }
  };
  for (int tail = 0; tail < 4; ++tail) {
    // Add an ignored, large private table. Its odd byte counts exercise all
    // trailing padding cases and force overflow beyond a small fixture face.
    const auto& native = prepared.native;
    const int count = get16(native, 4);
    const int header = 12 + 16 * count;
    QByteArray extended(native.size() + 16, '\0');
    std::copy_n(native.constData(), header, extended.data());
    std::copy(native.constData() + header, native.constData() + native.size(), extended.data() + header + 16);
    qToBigEndian<quint16>(count + 1, extended.data() + 4);
    for (int i = 0; i < count; ++i)
      qToBigEndian<quint32>(get32(native, 12 + 16 * i + 8) + 16, extended.data() + 12 + 16 * i + 8);
    const auto offset = extended.size();
    const int length = 1024 * 1024 + tail;
    qToBigEndian<quint32>(0x54455354, extended.data() + header);
    qToBigEndian<quint32>(offset, extended.data() + header + 8);
    qToBigEndian<quint32>(length, extended.data() + header + 12);
    QByteArray glyphs(length, Qt::Uninitialized);
    for (int i = 0; i < length; ++i) glyphs[i] = char((i * 37 + 127) % 256);
    extended += glyphs;
    const auto original = extended;
    verify(isolateThemeFont(extended, "MuffinChecksum", 700, true));
    require(extended == original, "private table views never modify the original font bytes");
  }
  const auto before = prepared.native;
  verify(isolateThemeFont(prepared.native, "MuffinPrepared", 400, false));
  verify(isolateThemeFont(prepared.native, "MuffinPrepared", 700, true));
  require(prepared.native == before, "descriptor/name rewrites keep prepared font source immutable");
  const auto registered = registerPrivateFont(prepared, "MuffinPrepared", 400, false);
  require(!registered.isEmpty() && registerPrivateFont(source, "MuffinPrepared", 400, false) == registered,
          "raw and prepared registration paths use the same content identity");
}

void declarations() {
  const auto sheet =
      CssThemeParser::parse("p{font-weight:450;font-weight:1001;font-style:italic;font-style:broken}", {});
  CssElement p;
  p.tag = "p";
  const auto style = CssThemeMapper::projectComputedStyle("p", CssComputedStyleEngine(sheet).styleFor(p));
  require(style.text.fontWeight == 450 && style.text.italic,
          "invalid font declarations preserve valid cascade fallback");
  const auto families =
      font_rendering::cssFamilyList("'Fixture, Comma', serif, 'Missing'", font_rendering::documentFallbackTail());
  require(families.front() == "Fixture, Comma" && families[1] == font_rendering::serifFamily(),
          "family quoting and generic order survive projection");
  require(font_rendering::cssFamilyList("'serif'", {}).front() == "serif", "quoted generic name stays literal");
  const auto both = font_rendering::cssFamilyList("'serif', serif", {});
  require(both.size() >= 2 && both.front() == "serif" && both[1] == font_rendering::serifFamily(),
          "quoted and unquoted generic names remain distinct");
  require(font_rendering::cssFamilyList("'Family\\27 Name'", {}).front() == "Family'Name",
          "CSS escapes in family names are decoded");
}
void conditionalFontsAndAssets() {
  QTemporaryDir dir;
  const auto path = dir.filePath("conditional.css");
  const auto firstFace =
      QString("@font-face{font-family:'Conditional';src:url('%1/tests/fixtures/theme/fonts/latex-regular.woff2')}")
          .arg(MUFFIN_SOURCE_DIR);
  const auto secondFace =
      QString("@font-face{font-family:'Conditional';src:url('%1/tests/fixtures/theme/fonts/abyss.woff2')}")
          .arg(MUFFIN_SOURCE_DIR);
  write(path, (firstFace + "@media(max-width:500px){" + secondFace + "}p{font-family:Conditional}").toUtf8());
  auto theme = RenderTheme::fromDefinition(ThemeDefinition::fromCss(path, "conditional"));
  const auto wide = theme.fontAliases();
  require(theme.updateForViewport(400, 600) && theme.fontAliases() != wide,
          "font-face media bucket activates different font bytes");
  require(theme.updateForViewport(800, 600) && theme.fontAliases() == wide,
          "leaving font media bucket restores its previous family");
  const auto copy = [&](const QString& name) {
    QFile file(QStringLiteral(MUFFIN_SOURCE_DIR "/tests/fixtures/theme/fonts/") + name);
    require(file.open(QIODevice::ReadOnly), "reload source font");
    write(dir.filePath("font.woff2"), file.readAll());
  };
  copy("latex-regular.woff2");
  write(dir.filePath("reload.css"),
        "@font-face{font-family:Reload;src:url(font.woff2)}p{font-family:Reload;color:#222}");
  ThemeManager manager(dir.path());
  require(manager.setTheme("reload"), "select reload test theme");
  const auto first = manager.currentDefinition().fontAliases;
  copy("abyss.woff2");
  require(manager.setTheme("reload") && manager.currentDefinition().fontAliases != first,
          "font content changes invalidate cached theme even when CSS path and text stay the same");
  QFile nativeSource(QStringLiteral(MUFFIN_SOURCE_DIR "/tests/fixtures/theme/fonts/latex-regular.woff2"));
  require(nativeSource.open(QIODevice::ReadOnly), "read font for same-size replacement");
  const auto native = decodeThemeWebFont(nativeSource.readAll());
  const auto faceA = isolateThemeFont(native, "RevisionA", 400, false);
  const auto faceB = isolateThemeFont(native, "RevisionB", 400, false);
  require(!faceA.isEmpty() && faceA.size() == faceB.size() && faceA != faceB,
          "same-size native faces have different contents");
  const auto fontPath = dir.filePath("font.woff2");
  write(fontPath, faceA);
  const auto beforeReplacement = manager.currentDefinition();
  const auto timestamp = QFileInfo(fontPath).lastModified();
  write(fontPath, faceB);
  QFile restored(fontPath);
  require(restored.open(QIODevice::ReadWrite) && restored.setFileTime(timestamp, QFileDevice::FileModificationTime),
          "restore font timestamp after same-size replacement");
  restored.close();
  const auto afterReplacement = manager.currentDefinition();
  require(afterReplacement.sourceSheet != beforeReplacement.sourceSheet &&
              afterReplacement.fontAliases != beforeReplacement.fontAliases,
          "independent reads detect same-size same-time font edits and replace private family identities");
  QFile source(QStringLiteral(MUFFIN_SOURCE_DIR "/tests/fixtures/theme/fonts/latex-regular.woff2"));
  require(source.open(QIODevice::ReadOnly), "embedded font source");
  write(path, "@font-face{font-family:Embedded;src:url(data:font/woff2;base64," + source.readAll().toBase64() +
                  ")}p{font-family:Embedded}");
  require(ThemeDefinition::fromCss(path, "embedded").fontAliases.contains("embedded"),
          "embedded webfont uses the same decoder and registry");
}
}  // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  require(argc == 2, "font mapping browser fixture argument");
  QFile file(QString::fromLocal8Bit(argv[1]));
  require(file.open(QIODevice::ReadOnly), "read font browser fixture");
  const auto root = QJsonDocument::fromJson(file.readAll()).object(), hashes = root["hashes"].toObject();
  for (auto it = hashes.begin(); it != hashes.end(); ++it) {
    QFile source(QStringLiteral(MUFFIN_SOURCE_DIR "/") + it.key());
    require(source.open(QIODevice::ReadOnly), "fixed font source exists");
    require(QString::fromLatin1(QCryptographicHash::hash(source.readAll(), QCryptographicHash::Sha256).toHex()) ==
                it.value().toString(),
            "font oracle bytes unchanged");
  }
  runTest("browser font mapping", [&] { browserFonts(root); });
  runTest("isolated font reload", isolationAndReload);
  runTest("prepared font checksums and identity", preparedFontChecksumsAndIdentity);
  runTest("font declarations", declarations);
  runTest("conditional fonts and asset reload", conditionalFontsAndAssets);
  return 0;
}
