#include "export/ExportOptions.h"
#include "export/NativeExporter.h"
#include "document/DocumentSession.h"
#include "../TestUtils.h"
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryDir>

using namespace muffin;

namespace {
QByteArray read(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "read exported file");
  return file.readAll();
}

void pdfPageSettingsAndHeadingBreaks() {
  DocumentSession document;
  document.setMarkdownText("# First\n\nAlpha\n\n# Second\n\nBeta\n", false);
  QSettings().setValue("export/pdf/paper", "A5");
  QSettings().setValue("export/pdf/landscape", true);
  QSettings().setValue("export/pdf/margin", 10.0);
  auto options = pdfExportOptions();
  require(options.pageLayout.orientation() == QPageLayout::Landscape && options.pageLayout.margins().left() == 10,
          "PDF settings become a real page layout");
  QTemporaryDir folder;
  const auto plain = folder.filePath("plain.pdf");
  auto theme = RenderTheme::defaultTheme();
  require(exportNativePdf(plain, document.document(), theme, {}, options), "export ordinary PDF");
  const auto data = QString::fromLatin1(read(plain));
  const QRegularExpression pages("/Type\\s*/Page\\b");
  const auto countPages = [&](const QString& text) {
    auto it = pages.globalMatch(text);
    int count = 0;
    while (it.hasNext()) {
      it.next();
      ++count;
    }
    return count;
  };
  require(countPages(data) == 1, "short note fits one PDF page");
  const auto media = QRegularExpression("/MediaBox\\s*\\[\\s*0\\s+0\\s+([0-9.]+)\\s+([0-9.]+)").match(data);
  require(media.hasMatch() && qAbs(media.captured(1).toDouble() - 595) < 2 &&
              qAbs(media.captured(2).toDouble() - 420) < 2,
          "PDF has A5 landscape dimensions in points");
  options.breakOnHeading = true;
  const auto split = folder.filePath("split.pdf");
  require(exportNativePdf(split, document.document(), theme, {}, options), "export heading-paginated PDF");
  require(countPages(QString::fromLatin1(read(split))) == 2, "level-one headings really introduce page breaks");
}

void imageDimensionsAndContent() {
  DocumentSession document;
  document.setMarkdownText("# Image\n\nRendered **content**\n", false);
  QTemporaryDir folder;
  const auto path = folder.filePath("note.png");
  ImageExportOptions options;
  options.width = 640;
  options.scale = 2;
  auto theme = RenderTheme::defaultTheme();
  require(exportNativeImage(path, document.document(), theme, {}, options), "export native PNG");
  QImage image(path);
  require(!image.isNull() && image.width() == 1280 && image.height() > 50, "image applies width and output scale");
  bool hasInk = false;
  const auto background = image.pixel(0, 0);
  for (int y = 0; y < image.height() && !hasInk; ++y)
    for (int x = 0; x < image.width(); ++x)
      if (image.pixel(x, y) != background) {
        hasInk = true;
        break;
      }
  require(hasInk, "PNG contains painted document content");
}

void formatOptionsAndQuotedArguments() {
  QSettings().setValue("export/docx/referenceDocument", "/notes/style template.docx");
  QSettings().setValue("export/docx/toc", true);
  QSettings().setValue("export/docx/tocDepth", 2);
  QSettings().setValue("export/docx/arguments", "--metadata subtitle=\"hello world\"");
  QSettings().setValue("export/epub/css", "/notes/book.css");
  QStringList args;
  require(pandocExportArguments(ExportFormat::Docx, "gfm", "out.docx", pandocExportOptions(ExportFormat::Docx), &args),
          "build Word arguments");
  require(args.contains("/notes/style template.docx") && args.contains("--toc-depth=2") &&
              args.contains("subtitle=hello world") && !args.contains("--css"),
          "Word keeps quoted paths, TOC and its own options");
  require(pandocExportArguments(ExportFormat::Epub, "gfm", "out.epub", pandocExportOptions(ExportFormat::Epub), &args),
          "build EPUB arguments");
  require(args.contains("--css") && args.contains("/notes/book.css") && !args.contains("--reference-doc") &&
              !args.contains("--toc"),
          "EPUB settings do not inherit Word settings");
  for (const auto& argument : QStringList{"--output=other.docx", "--out=other.docx", "--ou=other.docx", "-oother.docx",
                                          "-sNoother.docx", "--to=html", "--from=gfm", "--defaults=settings.yaml"}) {
    PandocExportOptions options;
    options.extraArguments = argument;
    require(!pandocExportArguments(ExportFormat::Docx, "gfm", "out.docx", options, &args),
            "reject arguments which bypass managed export destinations");
  }
  PandocExportOptions options;
  options.extraArguments = "--toc --toc-depth=2 -sVfoo=bar -sfalse";
  require(pandocExportArguments(ExportFormat::Docx, "gfm", "out.docx", options, &args),
          "custom TOC arguments are allowed");
}
}

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QCoreApplication::setOrganizationName("MuffinTests");
  QCoreApplication::setApplicationName("ExportOptions");
  runTest("pdfPageSettingsAndHeadingBreaks", pdfPageSettingsAndHeadingBreaks);
  runTest("imageDimensionsAndContent", imageDimensionsAndContent);
  runTest("formatOptionsAndQuotedArguments", formatOptionsAndQuotedArguments);
}
