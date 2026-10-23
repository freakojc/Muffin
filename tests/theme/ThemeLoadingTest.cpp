#include "theme/ThemeManager.h"
#include "theme/CssThemeParser.h"
#include "theme/CssThemeMapper.h"
#include "theme/ThemeResourceDigest.h"
#include "diagnostics/ThemePerformance.h"
#include "document/DocumentSession.h"
#include "document/MarkdownDocument.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QFontMetricsF>
#include <cstdio>
#include <cstdlib>

using namespace muffin;
namespace {
void require(bool ok, const char* message) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
  }
}
void write(const QString& path, const QByteArray& text) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "write theme fixture");
}
void overwritePreservingTime(const QString& path, const QByteArray& text) {
  const auto before = QFileInfo(path);
  const auto timestamp = before.lastModified();
  require(before.size() == text.size(), "replacement fixture preserves file size");
  write(path, text);
  QFile file(path);
  require(file.open(QIODevice::ReadWrite) && file.setFileTime(timestamp, QFileDevice::FileModificationTime),
          "replacement fixture preserves modification time");
}
void testResourceDigest() {
  QTemporaryDir dir;
  const auto path = dir.filePath(QString::fromUtf8("字体资源.bin"));
  write(path, {});
  require(themeResourceDigest(path).toHex() == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "readable empty file has the standard SHA-256 digest");
  require(themeContentDigest({}) == themeResourceDigest(path), "empty memory and file inputs use the same SHA-256");
  write(path, "abc");
  require(themeResourceDigest(path).toHex() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "resource digest matches the standard abc vector");
  require(themeContentDigest(QByteArrayView("abc", 3)) == themeResourceDigest(path),
          "memory and streaming backends agree on the standard abc vector");
  for (const int size : {1, 55, 56, 63, 64, 65, 262143, 262144, 262145, 1024 * 1024 + 3}) {
    QByteArray bytes(size, Qt::Uninitialized);
    for (int i = 0; i < size; ++i)
      bytes[i] = char((i * 137 + i / 251) & 255);
    write(path, bytes);
    require(themeResourceDigest(path) == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256),
            "binary resources match independent Qt SHA-256 across block and streaming boundaries");
    const auto original = bytes;
    const auto padded = QByteArray("x") + bytes + "y";
    const QByteArrayView view(padded.constData() + 1, bytes.size());
    require(themeContentDigest(view) == themeResourceDigest(path) && bytes == original &&
                padded == QByteArray("x") + original + "y",
            "unaligned borrowed memory hashes only its view and preserves the source bytes");
    bytes[size / 2] = char(quint8(bytes[size / 2]) ^ 0x40);
    overwritePreservingTime(path, bytes);
    require(themeResourceDigest(path) == QCryptographicHash::hash(bytes, QCryptographicHash::Sha256),
            "digest reads current content despite identical size and timestamp");
    require(themeContentDigest(bytes) == themeResourceDigest(path) &&
                themeContentDigest(view) != themeContentDigest(bytes),
            "changed font bytes invalidate memory digests without changing an earlier borrowed view");
  }
  QFile embedded(":/themes/github.css");
  require(embedded.open(QIODevice::ReadOnly), "open embedded theme resource");
  require(themeResourceDigest(embedded.fileName()) ==
              QCryptographicHash::hash(embedded.readAll(), QCryptographicHash::Sha256),
          "resource digest also handles Qt resource files");
  require(themeResourceDigest(dir.path()).isEmpty(), "directory is not accepted as a readable resource");
  require(QFile::remove(path) && themeResourceDigest(path).isEmpty(), "deleted resources have an absent digest");
  write(path, "abc");
  require(!themeResourceDigest(path).isEmpty(), "recreated resources are read without a negative cache");
}

void testSelectionReadScope() {
  QTemporaryDir dir;
  const auto path = dir.filePath("custom.css");
  write(path, "p{color:#123456}");
  write(dir.filePath("other.css"), "p{color:#abcdef}");
  ThemeManager manager(dir.path());
  require(manager.setTheme("custom") && manager.setTheme("other"), "warm selection fixtures");
  ThemeDefinition selected;
  int callbacks = 0;
  auto connection = QObject::connect(&manager, &ThemeManager::themeChanged, &manager, [&](const QString& id) {
    if (id != "custom")
      return;
    ++callbacks;
    selected = manager.currentDefinition();
    overwritePreservingTime(path, "p{color:#654321}");
    for (int i = 0; i < 4; ++i) {
      require(manager.definition(id).sourceSheet == selected.sourceSheet &&
                  manager.currentDefinition().colors.text == QColor("#123456"),
              "synchronous editor/chrome reads share the verified selection snapshot");
      auto theme = manager.currentTheme();
      theme.updateForViewport(800, 600);
      require(theme.textColorForElement("p") == QColor("#123456"), "render snapshot agrees with selection chrome");
    }
  });
  require(manager.setTheme("custom") && callbacks == 1, "selection notifies synchronous consumers");
  QObject::disconnect(connection);
  require(manager.currentDefinition().colors.text == QColor("#654321") &&
              manager.currentDefinition().sourceSheet != selected.sourceSheet,
          "the next independent read revalidates a same-size same-time replacement");
  require(selected.colors.text == QColor("#123456"), "previous selection remains immutable");

  require(manager.setTheme("other"), "switch away before queued notification");
  bool queued = false;
  connection = QObject::connect(
      &manager, &ThemeManager::themeChanged, &manager,
      [&](const QString& id) {
        if (id != "custom")
          return;
        queued = true;
        require(manager.currentDefinition().colors.text == QColor("#112233"),
                "queued reads after selection revalidate current resource contents");
      },
      Qt::QueuedConnection);
  require(manager.setTheme("custom"), "select before queued read");
  overwritePreservingTime(path, "p{color:#112233}");
  QCoreApplication::processEvents();
  QObject::disconnect(connection);
  require(queued, "queued selection notification was dispatched");

  require(manager.setTheme("other"), "switch away before nested selection");
  connection = QObject::connect(&manager, &ThemeManager::themeChanged, &manager, [&](const QString& id) {
    if (id != "custom")
      return;
    const auto outer = manager.currentDefinition();
    overwritePreservingTime(path, "p{color:#445566}");
    require(manager.setTheme("other"), "reentrant selection owns an independent verification scope");
    require(manager.currentDefinition().colors.text == QColor("#abcdef"), "nested current theme remains authoritative");
    require(manager.definition("custom").sourceSheet == outer.sourceSheet,
            "returning from a nested selection restores the outer immutable snapshot");
  });
  require(manager.setTheme("custom") && manager.currentThemeName() == "other",
          "nested selection restores scope safely");
  QObject::disconnect(connection);
  require(manager.definition("custom").colors.text == QColor("#445566"),
          "nested operations do not leak pinned definitions");

  require(manager.setTheme("custom"), "select before reload");
  overwritePreservingTime(path, "p{color:#556677}");
  connection = QObject::connect(&manager, &ThemeManager::themeChanged, &manager, [&](const QString&) {
    selected = manager.currentDefinition();
    overwritePreservingTime(path, "p{color:#778899}");
  });
  bool listed = false;
  const auto listedConnection = QObject::connect(&manager, &ThemeManager::themesChanged, &manager, [&] {
    listed = true;
    require(manager.currentDefinition().sourceSheet == selected.sourceSheet,
            "reload selection and list notifications share one verified definition");
  });
  manager.reloadCustomThemes();
  QObject::disconnect(connection);
  QObject::disconnect(listedConnection);
  require(listed && selected.colors.text == QColor("#556677") &&
              manager.currentDefinition().colors.text == QColor("#778899"),
          "reload operation ends before independent content validation");
}
void testLazyLoadingAndContentInvalidation() {
  QTemporaryDir dir;
  const auto path = dir.filePath("custom.css");
  write(path, ":root{--muffin-label:'Authored label'}p{color:#123456}");
  write(dir.filePath("invalid.json"), "not JSON");
  write(dir.filePath("github.css"), "p{color:red}");
  ThemeManager manager(dir.path());
  const auto& entries = manager.themes();
  require(entries.size() == 7 && entries.front().id == "github",
          "scan filenames without loading or shadowing built-ins");
  require(entries[5].label == "Custom", "unselected theme has filename metadata, not parsed CSS");
  require(manager.availableThemes().contains("invalid"), "listing does not parse an invalid unselected JSON file");
  require(manager.setTheme("custom"), "select and load CSS");
  const auto before = manager.currentDefinition();
  require(before.colors.text == QColor("#123456"), "selected CSS drives the theme");
  require(manager.themes()[5].label.contains("Authored label"), "loaded theme retains authored label");
  require(manager.setTheme("custom") && manager.currentDefinition().sourceSheet == before.sourceSheet,
          "unchanged selection reuses the loaded definition");
  auto prototype = manager.currentTheme();
  prototype.updateForViewport(800, 600);
  DocumentSession document;
  document.setMarkdownText("paragraph\n", false);
  const auto& paragraph = *document.document().root().children().front();
  require(prototype.textColorForElement("p", &paragraph) == QColor("#123456"), "first projected theme applies CSS");
  enableThemePerformance(true);
  resetThemePerformance();
  auto repeated = manager.currentTheme();
  repeated.updateForViewport(800, 600);
  const auto projectionWork = themePerformanceSamples()[size_t(ThemePerfStage::Css)].work;
  enableThemePerformance(false);
  require(projectionWork == 0 && repeated.styleCacheStats().elements == 0,
          "repeated theme projection reuses prepared data without document adapters or CSS evaluation");
  require(repeated.textColorForElement("p", &paragraph) == QColor("#123456"), "detached view resolves its own CSS");
  repeated.invalidateDocumentStyles();
  require(prototype.textColorForElement("p", &paragraph) == QColor("#123456"),
          "view invalidation cannot mutate another snapshot");

  QFile file(path);
  const auto timestamp = QFileInfo(file).lastModified();
  write(path, ":root{--muffin-label:'Authored label'}p{color:#654321}");
  require(file.open(QIODevice::ReadWrite), "open theme to restore timestamp");
  require(file.setFileTime(timestamp, QFileDevice::FileModificationTime), "preserve timestamp in regression fixture");
  file.close();
  int changes = 0;
  QObject::connect(&manager, &ThemeManager::themeChanged, [&] { ++changes; });
  require(manager.setTheme("custom"), "reselect changed theme");
  require(manager.currentDefinition().colors.text == QColor("#654321") && changes == 1,
          "same-size CSS with preserved timestamp invalidates and reapplies the current theme");
  require(manager.currentDefinition().sourceSheet != before.sourceSheet && before.colors.text == QColor("#123456"),
          "reloading replaces the snapshot without mutating previous themes");
  auto changed = manager.currentTheme();
  changed.updateForViewport(800, 600);
  require(changed.textColorForElement("p", &paragraph) == QColor("#654321"),
          "same-size CSS edits invalidate prepared projections too");
  require(!manager.setTheme("invalid") && manager.currentThemeName() == "custom",
          "invalid selection preserves current theme");
  require(!manager.setTheme("unknown"), "unknown theme cannot be selected");
  require(manager.definition("unknown").id == "github", "unknown definition falls back to GitHub");
  require(QFile::remove(path), "remove selected CSS");
  manager.reloadCustomThemes();
  require(manager.currentThemeName() == "github" && !manager.availableThemes().contains("custom"),
          "removing the active theme falls back and refreshes the list");
}
void testDependencyInvalidation() {
  QTemporaryDir dir;
  write(dir.filePath("custom.css"), "@import 'base.css';@import 'missing.css';p{color:var(--ink)}");
  write(dir.filePath("base.css"), ":root{--ink:#123456}p{background-image:url(texture.svg)}");
  ThemeManager manager(dir.path());
  require(manager.setTheme("custom"), "load theme with imports");
  const auto first = manager.currentDefinition();
  overwritePreservingTime(dir.filePath("base.css"), ":root{--ink:#654321}p{background-image:url(texture.svg)}");
  require(manager.setTheme("custom") && manager.currentDefinition().colors.text == QColor("#654321"),
          "imported CSS content invalidates the parent cache");
  write(dir.filePath("missing.css"), ":root{--ink:#abcdef}");
  require(manager.setTheme("custom") && manager.currentDefinition().colors.text == QColor("#abcdef"),
          "creating a formerly missing import invalidates the cache");
  const auto missingAsset = manager.currentDefinition().sourceSheet;
  write(dir.filePath("texture.svg"), "<svg xmlns='http://www.w3.org/2000/svg'/>");
  require(manager.setTheme("custom") && manager.currentDefinition().sourceSheet != missingAsset,
          "creating a formerly missing local resource invalidates the cache");
  const auto asset = manager.currentDefinition().sourceSheet;
  write(dir.filePath("texture.svg"), "<svg xmlns='http://www.w3.org/2000/svg' width='10'/>");
  require(manager.setTheme("custom") && manager.currentDefinition().sourceSheet != asset,
          "editing local resource content invalidates the cache");
  require(QFile::remove(dir.filePath("missing.css")), "remove imported CSS");
  manager.reloadCustomThemes();
  require(manager.currentDefinition().colors.text == QColor("#654321"), "reload notices removed imports");
  require(first.colors.text == QColor("#123456"), "previous dependency snapshot remains immutable");
}
void testVariableCacheInputs() {
  QHash<QString, QString> variables{{"--base", "#123456"},
                                    {"--ink", "var(--base)"},
                                    {"--fallback", "var(--absent, red)"},
                                    {"--quoted", "'var(--base)'"}};
  auto computed = CssThemeParser::computeCustomProperties(variables);
  require(computed["--ink"] == "#123456" && computed["--fallback"] == "red" && computed["--quoted"] == "'var(--base)'",
          "cache preserves substitution, fallbacks and literal strings");
  require(CssThemeParser::computeCustomProperties(variables) == computed, "repeated variable map retains semantics");
  require(CssThemeParser::computeCustomProperties(computed) == computed, "inherited computed maps remain idempotent");
  auto literal = computed;
  literal["--svg"] = QString("data:image/svg+xml,") + QString(500000, 'x') + "'var(--quoted)'";
  const auto original = literal;
  require(CssThemeParser::computeCustomProperties(literal) == original &&
              CssThemeParser::computeCustomProperties(literal) == original,
          "large literal resources and quoted variable tokens survive computed-map reuse");
  literal["--late"] = "var(--ink)";
  require(CssThemeParser::computeCustomProperties(literal)["--late"] == "#123456",
          "adding a live variable dependency invalidates an inherited computed-map hit");
  variables["--base"] = "#654321";
  require(CssThemeParser::computeCustomProperties(variables)["--ink"] == "#654321",
          "changed scope cannot reuse old variable result");
  variables["--absent"] = "blue";
  require(CssThemeParser::computeCustomProperties(variables)["--fallback"] == "blue",
          "new fallback target invalidates the result");
  variables["--base"] = "var(--ink)";
  computed = CssThemeParser::computeCustomProperties(variables);
  require(!computed.contains("--base") && !computed.contains("--ink"),
          "new cycles remain invalid after cached acyclic input");
  for (int i = 0; i < 40; ++i) {
    variables["--base"] = QString::number(i);
    require(CssThemeParser::computeCustomProperties(variables)["--ink"] == QString::number(i),
            "cache eviction preserves variable results");
  }
}
void testPreparedProjectionEnvironmentAndEviction() {
  QTemporaryDir dir;
  write(dir.filePath("responsive.css"), "p{color:#112233;font-size:16px}"
                                        "@media(min-width:850px){p{color:#445566;font-size:20px}}");
  ThemeManager manager(dir.path());
  require(manager.setTheme("responsive"), "select responsive theme");
  auto narrow = manager.currentTheme();
  narrow.updateForViewport(800, 600);
  const auto narrowColor = narrow.textColorForElement("p");
  auto wide = manager.currentTheme();
  wide.updateForViewport(900, 600);
  require(wide.textColorForElement("p") != narrowColor, "viewport media changes cannot reuse another projection");
  for (int i = 0; i < 12; ++i) {
    auto resized = manager.currentTheme();
    resized.updateForViewport(700 + i * 30, 600 + i);
  }
  auto narrowAgain = manager.currentTheme();
  narrowAgain.updateForViewport(800, 600);
  require(narrowAgain.textColorForElement("p") == narrowColor, "bounded projection eviction preserves CSS results");
  auto zoomed = manager.currentTheme(200, 20);
  zoomed.updateForViewport(900, 600);
  require(zoomed.textColorForElement("p") == narrowColor &&
              QFontMetricsF(zoomed.paragraphFont()).height() > QFontMetricsF(narrow.paragraphFont()).height(),
          "zoom and font settings use their own prepared environment");
  require(narrow.textColorForElement("p") == narrowColor, "other environment snapshots remain immutable");
}
void testPreparedEngineSourceAndEnvironmentInvalidation() {
  auto definition = CssThemeMapper::fromCss("body{color:#123456}p{font-size:16px}p:hover{color:red}"
                                            "@media(max-width:500px){p{font-size:20px;color:blue}}"
                                            "@media(prefers-color-scheme:dark){strong{color:yellow}}",
                                            "prepared", {});
  require(bool(definition.preparedEngine), "mapped themes retain a compiled immutable engine template");
  auto theme = RenderTheme::fromDefinition(definition);
  theme.updateForViewport(400, 600);
  const auto narrow = theme.textColorForElement("p");
  theme.updateForViewport(800, 600);
  require(narrow == QColor("blue") && theme.textColorForElement("p") == QColor("#123456"),
          "compiled selector reuse respects conditional declarations and environment values");
  const auto old = theme;
  definition.sourceSheet->mergeIn(CssThemeParser::parse("p{color:#654321;font-size:23px}", {}));
  auto changed = RenderTheme::fromDefinition(definition);
  changed.updateForViewport(800, 600);
  require(changed.textColorForElement("p") == QColor("#654321") && old.textColorForElement("p") == QColor("#123456"),
          "source revisions reject stale prepared compilation without changing previous snapshots");
  CssEnvironment env;
  env.viewportWidth = 400;
  auto contextual = CssThemeMapper::fromSheet(*definition.sourceSheet, "environment", env);
  auto restored = RenderTheme::fromDefinition(contextual);
  restored.updateForViewport(800, 600);
  require(restored.textColorForElement("p") == QColor("#654321"),
          "a prepared engine created in another environment does not leak its conditional cascade");
}

void testDeepDependenciesAndEviction() {
  QTemporaryDir dir;
  write(dir.filePath("custom.css"), "@import 'level0.css';p{color:var(--ink)}");
  for (int i = 0; i < 20; ++i)
    write(dir.filePath(QString("level%1.css").arg(i)),
          i == 19 ? QByteArray(":root{--ink:#123456}") : QString("@import 'level%1.css';").arg(i + 1).toUtf8());
  ThemeManager manager(dir.path());
  require(manager.setTheme("custom") && manager.currentDefinition().colors.text == QColor("#123456"),
          "load deep import chain");
  write(dir.filePath("level19.css"), ":root{--ink:#654321}");
  require(manager.setTheme("custom") && manager.currentDefinition().colors.text == QColor("#654321"),
          "dependency manifest must not truncate deep CSS import chains");
  const auto previous = manager.currentDefinition().sourceSheet;
  for (int i = 0; i < 9; ++i)
    write(dir.filePath(QString("other%1.css").arg(i)), "p{color:#123456}");
  manager.reloadCustomThemes();
  for (int i = 0; i < 9; ++i)
    require(manager.setTheme(QString("other%1").arg(i)), "select cache eviction fixture");
  require(manager.setTheme("custom") && manager.currentDefinition().sourceSheet != previous &&
              manager.currentDefinition().colors.text == QColor("#654321"),
          "evicted themes reload correctly");

  write(dir.filePath("native.json"), R"({"label":"Authored JSON","colors":{"text":"#123456"}})");
  manager.reloadCustomThemes();
  require(manager.setTheme("native") && manager.currentDefinition().label == "Authored JSON",
          "JSON themes load on demand too");
  write(dir.filePath("native.json"), R"({"label":"Authored JSON","colors":{"text":"#654321"}})");
  require(manager.setTheme("native") && manager.currentDefinition().colors.text == QColor("#654321"),
          "JSON content invalidates its cache");
}
void benchmarkInstalledThemes() {
  const auto directory = qEnvironmentVariable("MUFFIN_THEME_STARTUP_BENCH");
  if (directory.isEmpty())
    return;
  QElapsedTimer timer;
  timer.start();
  ThemeManager manager(directory);
  std::fprintf(stderr, "theme scan: %lld ms, %lld entries\n", timer.elapsed(),
               static_cast<long long>(manager.themes().size()));
  for (const auto& id : {"phycat-caramel", "vlook-fancy-dark", "vlook-fancy-dark"}) {
    timer.restart();
    const bool loaded = manager.setTheme(QString::fromLatin1(id));
    std::fprintf(stderr, "theme select %s: %lld ms, loaded=%d\n", id, timer.elapsed(), loaded);
    require(loaded, "benchmark theme loads successfully");
  }
}
} // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("MuffinTests");
  QCoreApplication::setApplicationName("ThemeLoading");
  testResourceDigest();
  testSelectionReadScope();
  testLazyLoadingAndContentInvalidation();
  testDependencyInvalidation();
  testVariableCacheInputs();
  testPreparedProjectionEnvironmentAndEviction();
  testPreparedEngineSourceAndEnvironmentInvalidation();
  testDeepDependenciesAndEviction();
  benchmarkInstalledThemes();
  return 0;
}
