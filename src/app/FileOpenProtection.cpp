#include "app/FileOpenProtection.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

QString muffin::file_open_protection::defaultTextExtensions() {
  return QStringLiteral("md, markdown, mdown, mkd, mkdn, mdx, txt, text, log, csv, tsv, "
                        "json, jsonl, xml, yaml, yml, toml, ini, cfg, conf, rst, adoc");
}

int muffin::file_open_protection::sizeLimitMb() {
  return qBound(1, QSettings().value(QLatin1String(kSizeLimitMb), kDefaultSizeLimitMb).toInt(),
                kMaximumSizeLimitMb);
}

muffin::file_open_protection::Warnings muffin::file_open_protection::warningsForFile(const QString& path) {
  const QFileInfo info(path);
  // Let the normal open path report missing/unreadable files without a misleading warning.
  if (!info.isFile() || !info.isReadable()) return {};
  QSettings settings;
  Warnings warnings;
  if (settings.value(QLatin1String(kExtensionEnabled), true).toBool()) {
    warnings.extension = true;
    const auto extensions = settings.value(QLatin1String(kTextExtensions), defaultTextExtensions()).toString()
        .split(QRegularExpression(QStringLiteral("[\\s,;，；]+")), Qt::SkipEmptyParts);
    for (QString extension : extensions) {
      if (extension.startsWith(QStringLiteral("*."))) extension.remove(0, 2);
      else if (extension.startsWith(QLatin1Char('.'))) extension.remove(0, 1);
      if (!extension.isEmpty() && info.fileName().endsWith(QLatin1Char('.') + extension, Qt::CaseInsensitive)) {
        warnings.extension = false;
        break;
      }
    }
  }
  warnings.size = settings.value(QLatin1String(kSizeEnabled), true).toBool()
      && info.size() > qint64(sizeLimitMb()) * 1024 * 1024;
  return warnings;
}
