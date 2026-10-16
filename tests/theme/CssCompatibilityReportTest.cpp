#include "theme/CssCompatibilityReport.h"
#include "theme/CssComputedStyleEngine.h"

#include "../TestUtils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>

using namespace muffin;

namespace {
void write(const QString& path, const QByteArray& text) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly) && file.write(text) == text.size(), "write temporary fixture");
}

QJsonObject finding(const QJsonObject& report, const QString& code, const QString& property = {}, const QString& scope = {}) {
  for (const auto& entry : report.value("findings").toArray()) {
    const auto object = entry.toObject();
    if (object.value("code") == code && (property.isEmpty() || object.value("property") == property) &&
        (scope.isEmpty() || object.value("scope") == scope)) return object;
  }
  return {};
}

void auditSourceAndCapabilities() {
  QTemporaryDir temp;
  require(temp.isValid() && QDir(temp.path()).mkdir("sub"), "temporary theme directory");
  write(temp.filePath("sub/base.css"), "/* base */\n@media (min-width: 700px) {\n p:has(> div .child) { color: red; }\n}\n"
                                     "@font-face {font-family:Missing;src:url('missing(font).woff2')}\n");
  write(temp.filePath("tile.png"), "resource existence check only");
  const QString css = QStringLiteral(
      "@import 'sub/base.css';\n"
      "@import url(https://invalid.example/theme.css);\n"
      "@import 'missing.css' print;\n"
      "@charset 'UTF-8';\n"
      "@layer cards { p { color: red } }\n"
      "@font-face {font-family:Remote;src:url(https://invalid.example/font.woff2)}\n"
      "@supports (display:contents) { p {counter-set:heading 1} }\n"
      "@media (prefers-color-scheme:dark) { p{ background-repeat:round } }\n"
      "p {font-family:serif;box-shadow:0 1px 2px black;color:red;display:contents;content:attr(count number);"
      "background-image:url(tile.png);background-repeat:space;background-blend-mode:multiply;mix-blend-mode:multiply;counter-reset:heading 0}"
      "p::before {content:'url(no-resource.svg)';background-image:url('data:image/svg+xml,<svg/>')}"
      ".ty-tooltip {counter-set:x 1} .typora-export p {display:contents}"
      "p:has(> div .child) {display:contents}"
      "p {color:var(--context);mix-blend-mode:var(--blend)}"
      "p {content:'url(fake.png)';background-image:url('data:image/unsupported;base64,") + QString(5000, 'A') + QStringLiteral("')}"
      "p {background-image:url('data:image/svg+xml,<svg/>')}"
      "p {width:bogus}p {width:bogus}p {width:bogus}p {width:bogus}");
  const auto source = temp.filePath("theme.css");
  const auto sheet = CssThemeParser::parse(css, temp.path(), source);
  require(sheet.rules().front().source.file == temp.filePath("sub/base.css"), "import retains owning filename");
  require(sheet.rules().front().source.line == 3 && sheet.rules().front().source.column == 2, "nested rule uses original line/column");
  require(sheet.atRules().front().source.line == 2, "at-rule location survives import");
  const auto report = CssCompatibilityReport::analyze(sheet, temp.path()).toJson();
  require(QJsonDocument(report).toJson() == QJsonDocument(CssCompatibilityReport::analyze(sheet, temp.path()).toJson()).toJson(),
          "report ordering is deterministic");
  require(!finding(report, "unsupported-at-rule", "@layer").isEmpty(), "ignored at-rule body is reported");
  require(!finding(report, "remote-resource-skipped", "import").isEmpty(), "remote import is never fetched");
  require(!finding(report, "import-condition-limits").isEmpty(), "ignored import media modifier is explicit");
  require(!finding(report, "missing-local-resource", "font").isEmpty(), "font path resolves against imported file");
  require(!finding(report, "remote-resource-skipped", "font").isEmpty(), "remote font source is retained for audit");
  require(finding(report, "unsupported-property", "counter-set").isEmpty(), "scoped counter-set is supported by the document evaluator");
  require(!finding(report, "unsupported-property-value", "display", "blocked-selector").isEmpty(), "blocked selector declarations are separate");
  require(!finding(report, "unverified-property", "font-family").isEmpty(), "missing validator is not missing painter");
  require(finding(report, "unsupported-property-value", "font-family").isEmpty(), "implemented font family is not falsely rejected");
  require(!finding(report, "context-dependent-value", "color").isEmpty(), "scoped custom properties are not guessed");
  require(finding(report, "unsupported-blend-mode").value("occurrences").toInt() == 1, "variable blend value is not falsely rejected");
  require(!finding(report, "unsupported-content-value").isEmpty(), "numeric attr has a diagnostic");
  require(!finding(report, "image-decoding-unverified").isEmpty(), "existing local background has a loading path, with decode left unverified");
  int svgBackgroundGaps = 0;
  for (const auto& entry : report.value("findings").toArray()) {
    const auto gap = entry.toObject();
    if (gap.value("code") == "unsupported-image-format" && gap.value("value").toString().startsWith("data:image/svg+xml"))
      svgBackgroundGaps += gap.value("occurrences").toInt();
  }
  require(svgBackgroundGaps == 0, "SVG works for generated icons and regular backgrounds");
  require(finding(report, "unsupported-property-value", "width").value("occurrences").toInt() == 4 &&
          finding(report, "unsupported-property-value", "width").value("examples").toArray().size() == 3,
          "duplicate findings retain counts with bounded examples");
  const auto json = QJsonDocument(report).toJson();
  require(!json.contains(QByteArray(500, 'A')) && json.contains("valueSha256"), "embedded payloads are abbreviated with a full digest");
  CssEnvironment dark;
  dark.dark = true;
  require(CssCompatibilityReport::analyze(sheet, temp.path(), dark).toJson().value("summary").toObject().value("activeRules").toInt() >
          report.value("summary").toObject().value("activeRules").toInt(), "condition environment is supplied explicitly");
  require(finding(report, "unsupported-background-repeat").isEmpty(), "space/round no longer produce unsupported repeat diagnostics");
  require(CssComputedStyleEngine::selectorSupport("p:has(> div .child)") == CssSelectorSupport::Unsupported &&
          CssComputedStyleEngine::selectorSupport("p:has(> img)") == CssSelectorSupport::Supported &&
          CssComputedStyleEngine::selectorSupport("p:has(input:checked)") == CssSelectorSupport::Unsupported &&
          CssComputedStyleEngine::selectorSupport(".ty-tooltip") == CssSelectorSupport::EditorOnly &&
          CssComputedStyleEngine::selectorSupport(".typora-export p") == CssSelectorSupport::ExportOnly,
          "audit shares selector compiler and exclusions");
  require(CssThemeParser::resourceUrls("'url(fake)' /* url(fake) */ url('real(x).png'), URL(real2.png)") ==
              QStringList{"real(x).png", "real2.png"}, "resource tokenizer skips strings/comments and retains quoted parentheses");
}

void realFixtureAndCli(const QString& fixture) {
  QFile file(fixture);
  require(file.open(QIODevice::ReadOnly), "read fixed VLOOK fixture");
  CssEnvironment dark;
  dark.dark = true; dark.viewportWidth = 960; dark.viewportHeight = 900;
  const auto source = QFileInfo(fixture).absoluteFilePath();
  const auto directory = QFileInfo(fixture).absolutePath();
  const auto sheet = CssThemeParser::parse(QString::fromUtf8(file.readAll()), directory, source);
  const auto expected = CssCompatibilityReport::analyze(sheet, directory, dark).toJson();
  require(expected.value("summary").toObject().value("rules").toInt() > 500, "real theme is audited, not a reduced substitute");
  require(!finding(expected, "remote-resource-skipped", "import").isEmpty(), "real VLOOK remote font stylesheet imports are visible in report");
  require(!finding(expected, "unsupported-selector").isEmpty(), "real VLOOK selector gaps are visible");
  const auto output = QDir(QCoreApplication::applicationDirPath()).filePath("vlook-fancy-dark-compatibility.json");
  QProcess cli;
  cli.start(QStringLiteral(MUFFIN_THEME_REPORT_PATH), {"--dark", "--width", "960", "--height", "900", "--output", output, source});
  require(cli.waitForFinished(30000) && cli.exitStatus() == QProcess::NormalExit && cli.exitCode() == 0, "offline report CLI succeeds");
  QFile report(output);
  require(report.open(QIODevice::ReadOnly) && QJsonDocument::fromJson(report.readAll()).object() == expected,
          "CLI exports same fixed VLOOK report as shared audit API");
  cli.start(QStringLiteral(MUFFIN_THEME_REPORT_PATH), {"--width", "nan", source});
  require(cli.waitForFinished(10000) && cli.exitCode() == 2, "CLI rejects invalid environment");
  cli.start(QStringLiteral(MUFFIN_THEME_REPORT_PATH), {source + ".missing"});
  require(cli.waitForFinished(10000) && cli.exitCode() == 2, "CLI reports unreadable input as an error");
}
}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  require(argc > 1, "fixed VLOOK CSS argument is required");
  runTest("source provenance and conservative capability classification", auditSourceAndCapabilities);
  runTest("fixed VLOOK report and offline CLI", [&] { realFixtureAndCli(QString::fromLocal8Bit(argv[1])); });
  return 0;
}
