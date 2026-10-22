#include "export/ExportOptions.h"

#include <QPageSize>
#include <QProcess>
#include <QSettings>
#include <QSizeF>

namespace muffin {

QString exportSettingsPrefix(ExportFormat format) {
  const char* name = "pdf";
  switch (format) {
    case ExportFormat::Pdf: name = "pdf"; break;
    case ExportFormat::Html: name = "html"; break;
    case ExportFormat::HtmlPlain: name = "htmlPlain"; break;
    case ExportFormat::Image: name = "image"; break;
    case ExportFormat::Docx: name = "docx"; break;
    case ExportFormat::Odt: name = "odt"; break;
    case ExportFormat::Rtf: name = "rtf"; break;
    case ExportFormat::Epub: name = "epub"; break;
    case ExportFormat::Latex: name = "latex"; break;
    case ExportFormat::MediaWiki: name = "mediawiki"; break;
    case ExportFormat::Rst: name = "rst"; break;
    case ExportFormat::Textile: name = "textile"; break;
    case ExportFormat::Opml: name = "opml"; break;
  }
  return QStringLiteral("export/") + QLatin1String(name) + QLatin1Char('/');
}

PdfExportOptions pdfExportOptions() {
  QSettings settings;
  const QString prefix = exportSettingsPrefix(ExportFormat::Pdf);
  const auto paper = settings.value(prefix + "paper", "A4").toString();
  QPageSize size(QPageSize::A4);
  if (paper == "Letter") size = QPageSize(QPageSize::Letter);
  else if (paper == "Legal") size = QPageSize(QPageSize::Legal);
  else if (paper == "A5") size = QPageSize(QPageSize::A5);
  else if (paper == "Custom") size = QPageSize(QSizeF(
      qBound(50.0, settings.value(prefix + "width", 210.0).toDouble(), 1000.0),
      qBound(50.0, settings.value(prefix + "height", 297.0).toDouble(), 1000.0)), QPageSize::Millimeter);
  const qreal margin = qBound(0.0, settings.value(prefix + "margin", 15.0).toDouble(), 20.0);
  PdfExportOptions options;
  options.pageLayout = QPageLayout(size, settings.value(prefix + "landscape", false).toBool()
      ? QPageLayout::Landscape : QPageLayout::Portrait, QMarginsF(margin, margin, margin, margin), QPageLayout::Millimeter);
  options.breakOnHeading = settings.value(prefix + "breakOnHeading", false).toBool();
  options.theme = settings.value(prefix + "theme", "current").toString();
  options.fontSize = qBound(10, settings.value(prefix + "fontSize", 16).toInt(), 40);
  return options;
}

ImageExportOptions imageExportOptions() {
  QSettings settings;
  const auto prefix = exportSettingsPrefix(ExportFormat::Image);
  return {qBound(320, settings.value(prefix + "width", 960).toInt(), 2400),
          qBound(1, settings.value(prefix + "scale", 1).toInt(), 3),
          qBound(10, settings.value(prefix + "fontSize", 16).toInt(), 40),
          settings.value(prefix + "theme", "current").toString()};
}

PandocExportOptions pandocExportOptions(ExportFormat format) {
  QSettings settings;
  const auto p = exportSettingsPrefix(format);
  PandocExportOptions options;
  options.referenceDocument = settings.value(p + "referenceDocument").toString();
  options.css = settings.value(p + "css").toString();
  options.title = settings.value(p + "title").toString();
  options.author = settings.value(p + "author").toString();
  options.extraArguments = settings.value(p + "arguments").toString();
  options.wrap = settings.value(p + "wrap", "none").toString();
  options.toc = settings.value(p + "toc", false).toBool();
  options.tocDepth = qBound(1, settings.value(p + "tocDepth", 3).toInt(), 6);
  options.numberSections = settings.value(p + "numberSections", false).toBool();
  return options;
}

bool pandocExportArguments(ExportFormat format, const QString& inputFormat,
                           const QString& target, const PandocExportOptions& o, QStringList* arguments) {
  QString writer;
  switch (format) {
    case ExportFormat::Docx: writer = "docx"; break;
    case ExportFormat::Odt: writer = "odt"; break;
    case ExportFormat::Rtf: writer = "rtf"; break;
    case ExportFormat::Epub: writer = "epub"; break;
    case ExportFormat::Latex: writer = "latex"; break;
    case ExportFormat::MediaWiki: writer = "mediawiki"; break;
    case ExportFormat::Rst: writer = "rst"; break;
    case ExportFormat::Textile: writer = "textile"; break;
    case ExportFormat::Opml: writer = "opml"; break;
    default: return false;
  }
  const auto extra = QProcess::splitCommand(o.extraArguments);
  for (const auto& arg : extra) {
    if (arg == "--") return false;
    if (arg.startsWith("--")) {
      // Pandoc accepts unique long-option abbreviations (e.g. --out).
      const auto option = arg.section('=', 0, 0);
      for (const auto& reserved : QStringList{"--output", "--to", "--write", "--from", "--read", "--defaults"})
        if (reserved.startsWith(option)) return false;
    } else if (arg.startsWith('-')) {
      // It also accepts short-option bundles such as -sNoFILE. Stop at an
      // option which consumes a value: -Vfoo must not be mistaken for -f.
      for (qsizetype i = 1; i < arg.size(); ++i) {
        const auto flag = arg[i];
        if (QStringLiteral("oftwrd").contains(flag)) return false;
        if (QStringLiteral("MVHBAFLTcD").contains(flag)) break;
        if (QStringLiteral("sNpi").contains(flag)
            && (arg.mid(i + 1) == "true" || arg.mid(i + 1) == "false")) break;
      }
    }
  }
  QStringList args{"-f", inputFormat, "-t", writer, "-o", target, "--standalone"};
  if ((format == ExportFormat::Docx || format == ExportFormat::Odt) && !o.referenceDocument.isEmpty())
    args << "--reference-doc" << o.referenceDocument;
  if (format == ExportFormat::Epub && !o.css.isEmpty()) args << "--css" << o.css;
  if (!o.title.isEmpty()) args << "--metadata" << "title=" + o.title;
  if (!o.author.isEmpty()) args << "--metadata" << "author=" + o.author;
  if (o.toc && (format == ExportFormat::Docx || format == ExportFormat::Odt || format == ExportFormat::Rtf
      || format == ExportFormat::Epub || format == ExportFormat::Latex))
    args << "--toc" << "--toc-depth=" + QString::number(o.tocDepth);
  if (o.numberSections && (format == ExportFormat::Docx || format == ExportFormat::Odt || format == ExportFormat::Latex))
    args << "--number-sections";
  args << "--wrap=" + (QStringList{"none", "auto", "preserve"}.contains(o.wrap) ? o.wrap : QStringLiteral("none"));
  args << extra;
  *arguments = args;
  return true;
}

}  // namespace muffin
