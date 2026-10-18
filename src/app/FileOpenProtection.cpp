#include "app/FileOpenProtection.h"

#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>

QString muffin::file_open_protection::defaultTextExtensions() {
  return QStringLiteral("md, markdown, mdown, mkd, mkdn, mdx, txt, text, log, csv, tsv, "
                        "json, jsonl, xml, yaml, yml, toml, ini, cfg, conf, rst, adoc");
}

qint64 muffin::file_open_protection::sizeLimitBytes() {
  QSettings settings;
  bool valid = false;
  const qint64 bytes = settings.value(QLatin1String(kSizeLimitBytes)).toLongLong(&valid);
  if (valid) return qBound(kMinimumSizeLimitBytes, bytes, kMaximumSizeLimitBytes);
  // Preserve thresholds saved by versions that only offered integer MB.
  return qint64(qBound(1, settings.value(QLatin1String(kSizeLimitMb), kDefaultSizeLimitMb).toInt(),
                      kMaximumSizeLimitMb)) * 1024 * 1024;
}

QStringList muffin::file_open_protection::normalizedTextExtensions(const QString& text) {
  QStringList result;
  const auto tokens = text.split(QRegularExpression(QStringLiteral("[\\s,;，；、]+")), Qt::SkipEmptyParts);
  for (QString extension : tokens) {
    if (extension.startsWith(QStringLiteral("*."))) extension.remove(0, 2);
    else if (extension.startsWith(QLatin1Char('.'))) extension.remove(0, 1);
    extension = extension.toCaseFolded();
    if (!extension.isEmpty() && !result.contains(extension)) result.append(extension);
  }
  return result;
}

muffin::file_open_protection::Warnings muffin::file_open_protection::warningsForFile(const QString& path) {
  const QFileInfo info(path);
  // Let the normal open path report missing/unreadable files without a misleading warning.
  if (!info.isFile() || !info.isReadable()) return {};
  QSettings settings;
  Warnings warnings;
  if (settings.value(QLatin1String(kExtensionEnabled), true).toBool()) {
    warnings.extension = true;
    const auto extensions = normalizedTextExtensions(settings.value(QLatin1String(kTextExtensions), defaultTextExtensions()).toString());
    for (const QString& extension : extensions) {
      if (!extension.isEmpty() && info.fileName().endsWith(QLatin1Char('.') + extension, Qt::CaseInsensitive)) {
        warnings.extension = false;
        break;
      }
    }
  }
  warnings.size = settings.value(QLatin1String(kSizeEnabled), true).toBool()
      && info.size() > sizeLimitBytes();
  return warnings;
}
