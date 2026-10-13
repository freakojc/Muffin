#include "theme/ThemeManager.h"
#include "theme/CssThemeParser.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

using namespace muffin;
namespace {
void require(bool ok, const char* message) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void write(const QString& path, const QByteArray& text) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "write theme fixture");
}
void testLazyLoadingAndContentInvalidation() {
  QTemporaryDir dir;
  const auto path = dir.filePath("custom.css");
  write(path, ":root{--muffin-label:'Authored label'}p{color:#123456}");
  write(dir.filePath("invalid.json"), "not JSON");
  write(dir.filePath("github.css"), "p{color:red}");
  ThemeManager manager(dir.path());
  const auto& entries = manager.themes();
  require(entries.size() == 7 && entries.front().id == "github", "scan filenames without loading or shadowing built-ins");
  require(entries[5].label == "Custom", "unselected theme has filename metadata, not parsed CSS");
  require(manager.availableThemes().contains("invalid"), "listing does not parse an invalid unselected JSON file");
  require(manager.setTheme("custom"), "select and load CSS");
  const auto before = manager.currentDefinition();
  require(before.colors.text == QColor("#123456"), "selected CSS drives the theme");
  require(manager.themes()[5].label.contains("Authored label"), "loaded theme retains authored label");
  require(manager.setTheme("custom") && manager.currentDefinition().sourceSheet == before.sourceSheet,
          "unchanged selection reuses the loaded definition");

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
  require(!manager.setTheme("invalid") && manager.currentThemeName() == "custom", "invalid selection preserves current theme");
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
  write(dir.filePath("base.css"), ":root{--ink:#654321}p{background-image:url(texture.svg)}");
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
  QHash<QString, QString> variables{{"--base", "#123456"}, {"--ink", "var(--base)"},
                                  {"--fallback", "var(--absent, red)"}, {"--quoted", "'var(--base)'"}};
  auto computed = CssThemeParser::computeCustomProperties(variables);
  require(computed["--ink"] == "#123456" && computed["--fallback"] == "red" && computed["--quoted"] == "'var(--base)'",
          "cache preserves substitution, fallbacks and literal strings");
  require(CssThemeParser::computeCustomProperties(variables) == computed, "repeated variable map retains semantics");
  variables["--base"] = "#654321";
  require(CssThemeParser::computeCustomProperties(variables)["--ink"] == "#654321", "changed scope cannot reuse old variable result");
  variables["--absent"] = "blue";
  require(CssThemeParser::computeCustomProperties(variables)["--fallback"] == "blue", "new fallback target invalidates the result");
  variables["--base"] = "var(--ink)";
  computed = CssThemeParser::computeCustomProperties(variables);
  require(!computed.contains("--base") && !computed.contains("--ink"), "new cycles remain invalid after cached acyclic input");
  for (int i = 0; i < 40; ++i) {
    variables["--base"] = QString::number(i);
    require(CssThemeParser::computeCustomProperties(variables)["--ink"] == QString::number(i), "cache eviction preserves variable results");
  }
}
void testDeepDependenciesAndEviction() {
  QTemporaryDir dir;
  write(dir.filePath("custom.css"), "@import 'level0.css';p{color:var(--ink)}");
  for (int i = 0; i < 20; ++i)
    write(dir.filePath(QString("level%1.css").arg(i)),
          i == 19 ? QByteArray(":root{--ink:#123456}") : QString("@import 'level%1.css';").arg(i + 1).toUtf8());
  ThemeManager manager(dir.path());
  require(manager.setTheme("custom") && manager.currentDefinition().colors.text == QColor("#123456"), "load deep import chain");
  write(dir.filePath("level19.css"), ":root{--ink:#654321}");
  require(manager.setTheme("custom") && manager.currentDefinition().colors.text == QColor("#654321"),
          "dependency manifest must not truncate deep CSS import chains");
  const auto previous = manager.currentDefinition().sourceSheet;
  for (int i = 0; i < 9; ++i) write(dir.filePath(QString("other%1.css").arg(i)), "p{color:#123456}");
  manager.reloadCustomThemes();
  for (int i = 0; i < 9; ++i) require(manager.setTheme(QString("other%1").arg(i)), "select cache eviction fixture");
  require(manager.setTheme("custom") && manager.currentDefinition().sourceSheet != previous &&
              manager.currentDefinition().colors.text == QColor("#654321"), "evicted themes reload correctly");

  write(dir.filePath("native.json"), R"({"label":"Authored JSON","colors":{"text":"#123456"}})");
  manager.reloadCustomThemes();
  require(manager.setTheme("native") && manager.currentDefinition().label == "Authored JSON", "JSON themes load on demand too");
  write(dir.filePath("native.json"), R"({"label":"Authored JSON","colors":{"text":"#654321"}})");
  require(manager.setTheme("native") && manager.currentDefinition().colors.text == QColor("#654321"), "JSON content invalidates its cache");
}
void benchmarkInstalledThemes() {
  const auto directory = qEnvironmentVariable("MUFFIN_THEME_STARTUP_BENCH");
  if (directory.isEmpty()) return;
  QElapsedTimer timer;
  timer.start();
  ThemeManager manager(directory);
  std::fprintf(stderr, "theme scan: %lld ms, %lld entries\n", timer.elapsed(), static_cast<long long>(manager.themes().size()));
  for (const auto& id : {"phycat-caramel", "vlook-fancy-dark", "vlook-fancy-dark"}) {
    timer.restart();
    const bool loaded = manager.setTheme(QString::fromLatin1(id));
    std::fprintf(stderr, "theme select %s: %lld ms, loaded=%d\n", id, timer.elapsed(), loaded);
    require(loaded, "benchmark theme loads successfully");
  }
}
}  // namespace
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("MuffinTests");
  QCoreApplication::setApplicationName("ThemeLoading");
  testLazyLoadingAndContentInvalidation();
  testDependencyInvalidation();
  testVariableCacheInputs();
  testDeepDependenciesAndEviction();
  benchmarkInstalledThemes();
  return 0;
}
