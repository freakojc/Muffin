#include "app/WorkspaceReplace.h"
#include "io/FileController.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include <QDateTime>
#include <QCoreApplication>

namespace {
QByteArray digest(const QByteArray& bytes) { return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex(); }
bool writeAtomic(const QString& path, const QByteArray& bytes, QString* error) {
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
    *error = file.errorString(); return false;
  }
  return true;
}
bool readExact(const QString& path, const QByteArray& expected) {
  QFile file(path);
  return file.open(QIODevice::ReadOnly) && file.read(expected.size() + 1) == expected && file.error() == QFileDevice::NoError;
}
bool ordinaryPath(const QString& root, const QString& path) {
  if (!muffin::WorkspaceFileIndex::contains(root, path)) return false;
  QString current = QFileInfo(path).absoluteFilePath();
  while (true) {
    const QFileInfo info(current);
    if (info.isSymLink() || info.isJunction()) return false;
    if (muffin::WorkspaceFileIndex::pathKey(current) == muffin::WorkspaceFileIndex::pathKey(root)) break;
    const auto parent = info.absolutePath();
    if (parent == current) return false;
    current = parent;
  }
  return true;
}
}

QVector<muffin::WorkspaceChange> muffin::WorkspaceReplace::prepare(
    const WorkspaceSearchResult& result, const QString& replacement, QStringList* skipped) {
  QVector<WorkspaceChange> changes;
  if (!result.complete || !result.error.isEmpty()) return changes;
  for (const auto& file : result.files) {
    if (!file.replaceable) { skipped->append(file.path); continue; }
    QString changed = file.text;
    bool tooLarge = false;
    for (auto it = file.hits.crbegin(); it != file.hits.crend(); ++it) {
      const auto expanded = DocumentSearch::expandReplacement(replacement, it->match, result.query.options.regularExpression);
      if (expanded.size() > 8 * 1024 * 1024 || changed.size() - it->match.length + expanded.size() > 8 * 1024 * 1024) {
        tooLarge = true; break;
      }
      changed.replace(it->match.start, it->match.length, expanded);
    }
    QByteArray after;
    if (tooLarge || !FileController::encodeTextBytes(changed, file.format, &after) || after.size() > 16 * 1024 * 1024) { skipped->append(file.path); continue; }
    if (after != file.original) changes.append({file.path, file.text, std::move(changed), file.original, std::move(after), int(file.hits.size())});
  }
  return changes;
}

muffin::WorkspaceReplaceReport muffin::WorkspaceReplace::apply(
    const QString& root, const QVector<WorkspaceChange>& changes,
    const QString& recoveryRoot, const SearchCancellation& cancel) {
  WorkspaceReplaceReport report;
  if (changes.isEmpty()) return report;
  if (!QDir().mkpath(recoveryRoot)) { report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Could not create recovery folder"); return report; }
  QLockFile lock(QDir(recoveryRoot).filePath("operation.lock"));
  if (!lock.tryLock(0)) { report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Another folder replacement is running"); return report; }
  const QString id = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmsszzz-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
  const QString directory = QDir(recoveryRoot).filePath(id);
  if (!QDir().mkpath(directory)) { report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Could not create recovery folder"); return report; }
  report.backupDirectory = directory;
  QJsonArray entries;
  // All originals and a durable journal precede ANY change to user files.
  for (qsizetype i = 0; i < changes.size(); ++i) {
    const auto& change = changes[i];
    const QString backup = QString::number(i) + ".original";
    if (!writeAtomic(QDir(directory).filePath(backup), change.before, &report.error)) return report;
    entries.append(QJsonObject{{"path", change.path}, {"backup", backup}, {"state", "unstarted"},
        {"before", QString::fromLatin1(digest(change.before))}, {"after", QString::fromLatin1(digest(change.after))}});
  }
  QJsonObject journal{{"version", 1}, {"root", QFileInfo(root).absoluteFilePath()}, {"files", entries}, {"restored", false}};
  const QString manifest = QDir(directory).filePath("manifest.json");
  if (QJsonDocument(journal).toJson().size() > 4 * 1024 * 1024) {
    report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Could not create recovery folder"); return report;
  }
  if (!writeAtomic(manifest, QJsonDocument(journal).toJson(), &report.error)) return report;
  for (qsizetype i = 0; i < changes.size(); ++i) {
    const auto& change = changes[i];
    if (cancel && cancel->load()) break;
    if (!ordinaryPath(root, change.path) || !QFileInfo(change.path).isWritable() || !readExact(change.path, change.before)) {
      report.skipped.append(change.path); continue;
    }
    auto entry = entries[i].toObject(); entry.insert("state", "pending"); entries[i] = entry;
    journal.insert("files", entries);
    if (!writeAtomic(manifest, QJsonDocument(journal).toJson(), &report.error)) break;
    QSaveFile output(change.path);
    if (!output.open(QIODevice::WriteOnly) || output.write(change.after) != change.after.size()) {
      report.skipped.append(change.path);
      entry.insert("state", "skipped"); entries[i] = entry; journal.insert("files", entries);
      if (!writeAtomic(manifest, QJsonDocument(journal).toJson(), &report.error)) break;
      continue;
    }
    // Recheck after staging the new bytes, before the atomic rename.
    if (!ordinaryPath(root, change.path) || !readExact(change.path, change.before) || !output.commit()) {
      report.skipped.append(change.path); entry.insert("state", "skipped");
    } else {
      report.changed.append(change.path); entry.insert("state", "applied");
    }
    entries[i] = entry; journal.insert("files", entries);
    if (!writeAtomic(manifest, QJsonDocument(journal).toJson(), &report.error)) break;
  }
  for (const auto& change : changes) {
    if (!report.changed.contains(change.path) && !report.skipped.contains(change.path)) report.skipped.append(change.path);
  }
  return report;
}

QString muffin::WorkspaceReplace::latestRecovery(const QString& recoveryRoot) {
  const auto directories = QDir(recoveryRoot).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
  for (const auto& name : directories) {
    const auto directory = QDir(recoveryRoot).filePath(name);
    QFile file(QDir(directory).filePath("manifest.json"));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 4 * 1024 * 1024) continue;
    const auto object = QJsonDocument::fromJson(file.readAll()).object();
    if (object.value("version").toInt() != 1 || object.value("restored").toBool()) continue;
    for (const auto& value : object.value("files").toArray()) {
      const auto state = value.toObject().value("state").toString();
      if (state == "pending" || state == "applied") return directory;
    }
  }
  return {};
}

muffin::WorkspaceReplaceReport muffin::WorkspaceReplace::restore(
    const QString& directory, const OpenSearchBuffers& buffers, const SearchCancellation& cancel) {
  WorkspaceReplaceReport report;
  report.backupDirectory = directory;
  QLockFile lock(QDir(QFileInfo(directory).absolutePath()).filePath("operation.lock"));
  if (!lock.tryLock(0)) { report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Another folder replacement is running"); return report; }
  QFile manifest(QDir(directory).filePath("manifest.json"));
  if (!manifest.open(QIODevice::ReadOnly) || manifest.size() > 4 * 1024 * 1024) { report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Could not read recovery journal"); return report; }
  auto journal = QJsonDocument::fromJson(manifest.readAll()).object(); manifest.close();
  if (journal.value("version").toInt() != 1 || journal.value("root").toString().isEmpty()) { report.error = QCoreApplication::translate("muffin::WorkspaceReplace", "Invalid recovery journal"); return report; }
  const auto root = journal.value("root").toString();
  bool complete = true;
  for (const auto& item : journal.value("files").toArray()) {
    if (cancel && cancel->load()) { complete = false; break; }
    const auto entry = item.toObject();
    const auto state = entry.value("state").toString();
    if (state != "pending" && state != "applied") continue;
    const auto path = entry.value("path").toString();
    const auto buffer = buffers.constFind(WorkspaceFileIndex::pathKey(path));
    const auto backupName = entry.value("backup").toString();
    if (QFileInfo(backupName).fileName() != backupName || !ordinaryPath(root, path)
        || (buffer != buffers.cend() && (buffer->modified || buffer->busy))) {
      report.skipped.append(path); complete = false; continue;
    }
    QFile original(QDir(directory).filePath(backupName)), current(path);
    if (!original.open(QIODevice::ReadOnly) || original.size() > 8 * 1024 * 1024
        || !current.open(QIODevice::ReadOnly) || current.size() > 16 * 1024 * 1024) {
      report.skipped.append(path); complete = false; continue;
    }
    const auto before = original.readAll(), now = current.readAll(); current.close();
    if (digest(before) != entry.value("before").toString().toLatin1()) {
      report.skipped.append(path); complete = false; continue;
    }
    if (digest(now) == digest(before)) continue;  // Already restored, or never committed.
    if (digest(now) != entry.value("after").toString().toLatin1()) {
      report.skipped.append(path); complete = false; continue;
    }
    if (buffer != buffers.cend()) {
      QString text; TextFileFormat format;
      if (!FileController::decodeTextBytes(now, &text, &format) || buffer->text != text) {
        report.skipped.append(path); complete = false; continue;
      }
    }
    QSaveFile output(path);
    if (!ordinaryPath(root, path) || !QFileInfo(path).isWritable() || !output.open(QIODevice::WriteOnly)
        || output.write(before) != before.size() || !readExact(path, now) || !ordinaryPath(root, path) || !output.commit()) {
      report.skipped.append(path); complete = false; continue;
    }
    report.changed.append(path);
  }
  if (complete) {
    journal.insert("restored", true);
    writeAtomic(QDir(directory).filePath("manifest.json"), QJsonDocument(journal).toJson(), &report.error);
  }
  return report;
}
