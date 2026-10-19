#include "app/WorkspaceSearch.h"
#include "io/FileController.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QElapsedTimer>
#include <QCoreApplication>

namespace {
QString snippet(const QString& text, qsizetype offset, qsizetype length, int* highlight) {
  const qsizetype begin = text.lastIndexOf('\n', qMax<qsizetype>(0, offset - 1));
  const qsizetype lineBegin = offset == 0 ? 0 : begin < 0 ? 0 : begin + 1;
  auto end = text.indexOf('\n', offset);
  if (end < 0) end = text.size();
  const auto start = qMax(lineBegin, offset - 70);
  const auto stop = qMin(end, qMax(offset + qMin<qsizetype>(length, 80), start + 160));
  *highlight = int(offset - start) + (start > lineBegin ? 1 : 0);
  return (start > lineBegin ? QString(QChar(0x2026)) : QString()) + text.mid(start, stop - start)
      + (stop < end ? QString(QChar(0x2026)) : QString());
}
}

muffin::WorkspaceSearchResult muffin::WorkspaceSearch::run(
    const WorkspaceQuery& query, const OpenSearchBuffers& buffers, const SearchCancellation& cancel,
    const std::function<void(const WorkspaceSearchFile&)>& progress) {
  WorkspaceSearchResult result;
  result.query = query;
  if (query.pattern.isEmpty()) return result;
  // PCRE2 honors inline limits; checking match validity below distinguishes a
  // resource failure from a successful search with zero matches.
  const QString expression = query.options.regularExpression
      ? QStringLiteral("(*LIMIT_MATCH=50000)(*LIMIT_DEPTH=500)") + query.pattern
      : QRegularExpression::escape(query.pattern);
  QRegularExpression regex(expression, query.options.caseSensitive
      ? QRegularExpression::NoPatternOption : QRegularExpression::CaseInsensitiveOption);
  if (!regex.isValid()) { result.error = regex.errorString(); result.complete = false; return result; }
  if (!QFileInfo(query.root).isDir()) { result.error = QCoreApplication::translate("muffin::WorkspaceSearch", "Folder is unavailable"); result.complete = false; return result; }
  const auto index = WorkspaceFileIndex::scan(query.root, query.extensions, cancel);
  result.complete = !index.truncated && !(cancel && cancel->load());
  result.skipped = index.unreadableDirectories;
  qint64 retainedBytes = 0;
  for (const auto& path : index.paths) {
    if (cancel && cancel->load()) { result.complete = false; break; }
    const auto relative = QDir(query.root).relativeFilePath(path);
    if (PathMatcher::score(query.includePath, relative) < 0 || (!query.excludePath.trimmed().isEmpty()
        && PathMatcher::score(query.excludePath, relative) >= 0)) continue;
    WorkspaceSearchFile file;
    file.path = path;
    const auto buffer = buffers.constFind(WorkspaceFileIndex::pathKey(path));
    if ((buffer == buffers.cend() && QFileInfo(path).size() > query.maxFileBytes) || (buffer != buffers.cend()
        && (buffer->busy || buffer->text.size() * 2 > query.maxFileBytes))) {
      result.skipped.append(path); continue;
    }
    QFile disk(path);
    QString diskText;
    if (QFileInfo(path).size() <= query.maxFileBytes && disk.open(QIODevice::ReadOnly)) {
      file.original = disk.read(query.maxFileBytes + 1);
      if (disk.error() != QFileDevice::NoError || file.original.size() > query.maxFileBytes
          || !FileController::decodeTextBytes(file.original, &diskText, &file.format) || diskText.contains(QChar(0))) {
        // An external disk edit must not hide a searchable unsaved buffer.
        if (buffer == buffers.cend()) { result.skipped.append(path); continue; }
      } else {
        QByteArray roundTrip;
        file.replaceable = FileController::encodeTextBytes(diskText, file.format, &roundTrip)
            && roundTrip == file.original && QFileInfo(path).isWritable();
      }
    } else if (buffer == buffers.cend()) { result.skipped.append(path); continue; }
    file.text = buffer == buffers.cend() ? diskText : buffer->text;
    file.modified = buffer != buffers.cend() && buffer->modified;
    file.replaceable = file.replaceable && !file.modified && (buffer == buffers.cend() || buffer->text == diskText);
    qsizetype lineOffset = 0;
    int line = 1;
    QElapsedTimer budget; budget.start();
    auto iterator = regex.globalMatch(file.text);
    while (true) {
      if ((cancel && cancel->load()) || budget.elapsed() > 2000) {
        result.complete = false; result.skipped.append(path); break;
      }
      if (!iterator.isValid()) { result.complete = false; result.error = QCoreApplication::translate("muffin::WorkspaceSearch", "Regular expression exceeded its resource limit"); break; }
      if (!iterator.hasNext()) break;
      const auto match = iterator.next();
      if (result.matches >= query.maxMatches) { result.complete = false; break; }
      WorkspaceHit hit;
      hit.match.start = match.capturedStart();
      hit.match.length = match.capturedLength();
      for (int i = 0; i <= match.lastCapturedIndex(); ++i) hit.match.captures.append(match.captured(i));
      while (lineOffset < hit.match.start) { if (file.text.at(lineOffset++) == '\n') ++line; }
      hit.line = line;
      hit.context = snippet(file.text, hit.match.start, hit.match.length, &hit.highlight);
      file.hits.append(std::move(hit));
      ++result.matches;
    }
    if (!file.hits.isEmpty()) {
      retainedBytes += file.original.size() + file.text.size() * 2;
      if (retainedBytes > 64 * 1024 * 1024) { result.matches -= file.hits.size(); result.complete = false; break; }
      if (progress) progress(file);
      result.files.append(std::move(file));
    }
    if (result.matches >= query.maxMatches || !result.error.isEmpty()) { result.complete = false; break; }
  }
  return result;
}
