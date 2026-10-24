#pragma once

#include "export/ExportFormat.h"

#include <QPageLayout>
#include <QStringList>

namespace muffin {

QString exportSettingsPrefix(ExportFormat format);

struct PdfExportOptions {
  QPageLayout pageLayout;
  bool breakOnHeading = false;
  QString theme;
  int fontSize = 16;
  QString header;
  QString footer;
  QString author;
  bool pageNumbers = false;
};

struct ImageExportOptions {
  int width = 960; // logical pixels, before output scaling
  int scale = 1;
  int fontSize = 16;
  QString theme;
};

struct PandocExportOptions {
  QString referenceDocument;
  QString css;
  QString title;
  QString author;
  QString extraArguments;
  QString wrap = QStringLiteral("none");
  bool toc = false;
  int tocDepth = 3;
  bool numberSections = false;
};

PdfExportOptions pdfExportOptions();
ImageExportOptions imageExportOptions();
PandocExportOptions pandocExportOptions(ExportFormat format);
// The caller owns input/output format and output path. Extra arguments cannot
// redirect them, so staging and cancellation never touch an existing output.
bool pandocExportArguments(ExportFormat format, const QString& inputFormat, const QString& target,
                           const PandocExportOptions& options, QStringList* arguments);

} // namespace muffin
