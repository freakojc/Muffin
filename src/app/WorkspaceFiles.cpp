#include "app/WorkspaceFiles.h"
#include "app/FileOpenProtection.h"
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>

namespace {
int segmentScore(const QString& needle, const QString& haystack) {
  if (needle.isEmpty()) return 0;
  if (needle == haystack) return 1000;
  if (haystack.startsWith(needle)) return 800;
  const auto position = haystack.indexOf(needle);
  if (position >= 0) return 600 - qMin<qsizetype>(position, 100);
  qsizetype matched = 0, first = -1, last = -1;
  for (qsizetype i = 0; i < haystack.size() && matched < needle.size(); ++i) {
    if (haystack.at(i) != needle.at(matched)) continue;
    if (first < 0) first = i;
    last = i;
    ++matched;
  }
  return matched == needle.size() ? 200 - qMin<qsizetype>(last - first, 150) : -1;
}
}
QStringList muffin::WorkspaceFileIndex::textExtensions() {
  return file_open_protection::normalizedTextExtensions(QSettings().value(
      QLatin1String(file_open_protection::kTextExtensions),
      file_open_protection::defaultTextExtensions()).toString());
}
QString muffin::WorkspaceFileIndex::pathKey(const QString& path) {
  if (path.isEmpty()) return {};
  const QFileInfo info(path);
  QString key = info.canonicalFilePath();
  if (key.isEmpty()) key = info.absoluteFilePath();
#ifdef Q_OS_WIN
  key = key.toCaseFolded();
#endif
  return QDir::cleanPath(key);
}
bool muffin::WorkspaceFileIndex::contains(const QString& root, const QString& path) {
  if (root.isEmpty() || path.isEmpty()) return false;
  const QString base = pathKey(root), candidate = pathKey(path);
  return candidate == base || candidate.startsWith(base.endsWith('/') ? base : base + '/');
}
muffin::WorkspaceFiles muffin::WorkspaceFileIndex::scan(
    const QString& root, const QStringList& extensions,
    const SearchCancellation& cancel, int limit) {
  WorkspaceFiles result;
  if (root.isEmpty() || !QFileInfo(root).isDir()) return result;
  static const QSet<QString> excluded = {".git", ".hg", ".svn", "node_modules", "build",
      "dist", "out", "target", ".cache", ".conan", ".venv", "__pycache__"};
  QStringList pending{QFileInfo(root).absoluteFilePath()};
  QSet<QString> visited;
  while (!pending.isEmpty() && !(cancel && cancel->load())) {
    const QString directory = pending.takeLast();
    const QFileInfo info(directory);
    if (info.isSymLink() || info.isJunction() || !contains(root, directory)) continue;
    const QString key = pathKey(directory);
    if (visited.contains(key)) continue;
    visited.insert(key);
    if (!info.isReadable()) { result.unreadableDirectories.append(directory); continue; }
    const auto entries = QDir(directory).entryInfoList(
        QDir::AllDirs | QDir::Files | QDir::NoDotAndDotDot, QDir::Name);
    for (const auto& entry : entries) {
      if (cancel && cancel->load()) return result;
      if (entry.isSymLink() || entry.isJunction()) continue;
      if (entry.isDir()) {
        if (!excluded.contains(entry.fileName().toCaseFolded())) pending.append(entry.absoluteFilePath());
      } else if (entry.isFile()) {
        bool allowed = false;
        for (const auto& ext : extensions) {
          if (entry.fileName().endsWith('.' + ext, Qt::CaseInsensitive)) { allowed = true; break; }
        }
        if (!allowed) continue;
        if (result.paths.size() >= limit) { result.truncated = true; return result; }
        result.paths.append(entry.absoluteFilePath());
      }
    }
  }
  return result;
}
int muffin::PathMatcher::score(const QString& query, const QString& relativePath) {
  QString normalizedQuery = query.trimmed().toCaseFolded();
  normalizedQuery.replace('\\', '/');
  if (normalizedQuery.isEmpty()) return 0;
  QString path = relativePath.toCaseFolded();
  path.replace('\\', '/');
  const auto parts = path.split('/', Qt::SkipEmptyParts);
  if (parts.isEmpty()) return -1;
  int score = 0;
  for (const auto& term : normalizedQuery.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts)) {
    const auto segments = term.split('/', Qt::SkipEmptyParts);
    if (segments.isEmpty()) continue;
    if (segments.size() == 1) {
      const int filename = segmentScore(term, parts.last());
      const int fullPath = segmentScore(term, path);
      if (filename < 0 && fullPath < 0) return -1;
      score += qMax(filename >= 0 ? filename + 1000 : -1, fullPath);
    } else {
      QVector<int> best(parts.size() + 1, 0);
      for (const auto& segment : segments) {
        QVector<int> next(parts.size() + 1, -1);
        for (qsizetype i = 0; i < parts.size(); ++i) {
          const int matched = segmentScore(segment, parts[i]);
          next[i + 1] = qMax(next[i], best[i] >= 0 && matched >= 0 ? best[i] + matched : -1);
        }
        best = std::move(next);
      }
      if (best.last() < 0) return -1;
      score += best.last();
    }
  }
  return score;
}
