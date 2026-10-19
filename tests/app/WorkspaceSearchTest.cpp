#include "app/WorkspaceSearch.h"
#include "app/WorkspaceReplace.h"
#include "app/SearchPanelWidget.h"
#include "io/FileController.h"
#include "../TestUtils.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QLabel>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeWidget>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QImage>
#include <QPainter>

using namespace muffin;
namespace {
void write(const QString& path, const QByteArray& bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
  require(file.open(QIODevice::WriteOnly), "write fixture"); require(file.write(bytes) == bytes.size(), "write all bytes");
}
QByteArray read(const QString& path) { QFile file(path); require(file.open(QIODevice::ReadOnly), "read fixture"); return file.readAll(); }
WorkspaceQuery query(const QString& root) { WorkspaceQuery q; q.root = root; q.pattern = QString::fromUtf8("术语"); q.extensions = {"md"}; return q; }
void search() {
  QTemporaryDir dir;
  const auto path = dir.filePath("deep/note.md");
  write(path, QString::fromUtf8("😀\n术语 in a link [label](术语)\n").toUtf8());
  auto q = query(dir.path()); auto cancel = std::make_shared<std::atomic_bool>(false);
  auto result = WorkspaceSearch::run(q, {}, cancel);
  require(result.complete && result.matches == 2 && result.files.size() == 1, "recursive content matches");
  require(result.files[0].hits[0].line == 2 && result.files[0].hits[0].match.start == 3, "UTF-16 offsets and source lines");
  OpenSearchBuffers buffers{{WorkspaceFileIndex::pathKey(path), {QString::fromUtf8("unsaved 术语"), true, false}}};
  result = WorkspaceSearch::run(q, buffers, cancel);
  require(result.matches == 1 && result.files[0].modified && !result.files[0].replaceable, "dirty buffer wins over disk, cannot batch replace");
  const auto diskOriginal = read(path);
  write(path, QByteArray("binary\0disk", 11));
  result = WorkspaceSearch::run(q, buffers, cancel);
  require(result.matches == 1 && !result.files[0].replaceable, "unreadable disk content does not hide the dirty buffer");
  write(path, diskOriginal);
  q.includePath = "other"; require(WorkspaceSearch::run(q, {}, cancel).matches == 0, "path filter");
  q.includePath.clear(); q.maxMatches = 1;
  require(!WorkspaceSearch::run(q, {}, cancel).complete, "truncated search cannot be replaced");
  q.maxMatches = 10000; q.options.regularExpression = true; q.pattern = "[";
  require(!WorkspaceSearch::run(q, {}, cancel).error.isEmpty(), "invalid regex is reported");
  q.pattern = "(?=.)|.";
  const auto zeroWidth = WorkspaceSearch::run(q, {}, cancel);
  const auto documentMatches = DocumentSearch::findAll(QString::fromUtf8("😀\n术语 in a link [label](术语)\n"), q.pattern, q.options);
  require(zeroWidth.matches == documentMatches.matches.size(), "zero-width regex semantics match current-document search");
  write(path, QByteArray(2000, 'a') + '!'); q.pattern = "(a+)+$";
  const auto limited = WorkspaceSearch::run(q, {}, cancel);
  require(!limited.complete && !limited.error.isEmpty(), "backtracking limit is an error, not zero matches");
}
void replacementAndRecovery() {
  QTemporaryDir dir, recovery;
  const auto path = dir.filePath("note.md"), other = dir.filePath("other.md"), mixed = dir.filePath("mixed.md");
  const auto original = QByteArray::fromHex("efbbbf") + QString::fromUtf8("术语\r\n术语").toUtf8();
  write(path, original); write(other, QString::fromUtf8("术语").toUtf8());
  write(mixed, QString::fromUtf8("术语\r\nsecond\n").toUtf8());
  auto cancel = std::make_shared<std::atomic_bool>(false);
  auto result = WorkspaceSearch::run(query(dir.path()), {}, cancel);
  QStringList skipped;
  auto changes = WorkspaceReplace::prepare(result, QString::fromUtf8("概念"), &skipped);
  require(changes.size() == 2 && skipped.contains(mixed), "mixed endings skipped; lossless formats admitted");
  require(read(path) == original, "preview does not write files");
  write(other, "external update");
  const auto report = WorkspaceReplace::apply(dir.path(), changes, recovery.path(), cancel);
  require(report.error.isEmpty() && report.changed.size() == 1 && report.skipped.contains(other), "stale file skipped at commit");
  require(read(path) == QByteArray::fromHex("efbbbf") + QString::fromUtf8("概念\r\n概念").toUtf8(), "BOM CRLF and missing final newline retained");
  require(WorkspaceReplace::latestRecovery(recovery.path()) == report.backupDirectory, "journal discovered after restart");
  OpenSearchBuffers dirty{{WorkspaceFileIndex::pathKey(path), {"draft", true, false}}};
  auto restored = WorkspaceReplace::restore(report.backupDirectory, dirty, cancel);
  require(restored.changed.isEmpty() && restored.skipped.contains(path), "restore preserves dirty buffers");
  const auto replaced = read(path); write(path, "new edit");
  restored = WorkspaceReplace::restore(report.backupDirectory, {}, cancel);
  require(read(path) == "new edit" && restored.skipped.contains(path), "restore refuses later edits");
  write(path, replaced);
  restored = WorkspaceReplace::restore(report.backupDirectory, {}, cancel);
  require(restored.changed.contains(path) && read(path) == original && read(other) == "external update", "restore exact bytes without overwriting skipped files");
  // A canceled operation never touches files but remains safe to recover.
  cancel->store(true);
  const auto canceled = WorkspaceReplace::apply(dir.path(), changes, recovery.path(), cancel);
  require(canceled.changed.isEmpty() && canceled.skipped.size() == changes.size() && read(path) == original, "cancel at file boundary reports unprocessed files");
  require(WorkspaceReplace::latestRecovery(recovery.path()).isEmpty(), "unstarted canceled operation does not hide earlier recovery");
}
void encodingAndCaptures() {
  for (const auto& encoding : {QStringLiteral("UTF-16LE"), QStringLiteral("UTF-16BE"), QStringLiteral("windows-1252")}) {
    QTemporaryDir directory, recovery;
    TextFileFormat format;
    format.encodingName = encoding; format.writeBom = encoding.startsWith("UTF-16");
    format.lineEnding = TextLineEnding::Cr; format.ensureTrailingNewline = true;
    const auto path = directory.filePath("note.md");
    QByteArray original;
    require(FileController::encodeTextBytes(QString::fromUtf8("café 12\ncafé 34"), format, &original), "encode legacy fixture");
    write(path, original);
    auto q = query(directory.path()); q.pattern = QString::fromUtf8("(café) (\\d+)"); q.options.regularExpression = true;
    auto cancel = std::make_shared<std::atomic_bool>(false);
    const auto result = WorkspaceSearch::run(q, {}, cancel);
    require(result.matches == 2 && result.files[0].replaceable, "UTF-16 and Western text are searched without losing format");
    QStringList skipped;
    const auto changes = WorkspaceReplace::prepare(result, "$2:$1", &skipped);
    QByteArray expected;
    require(FileController::encodeTextBytes(QString::fromUtf8("12:café\n34:café"), format, &expected), "encode expected replacement");
    require(changes.size() == 1 && changes[0].after == expected, "captures preserve encoding, CR endings, and absent final newline");
    const auto applied = WorkspaceReplace::apply(directory.path(), changes, recovery.path(), cancel);
    require(applied.changed.contains(path) && read(path) == expected, "encoded replacement committed");
    const auto restored = WorkspaceReplace::restore(applied.backupDirectory, {}, cancel);
    require(restored.changed.contains(path) && read(path) == original, "encoded original restored byte for byte");
  }
}
void panelGeneration() {
  QTemporaryDir dir; write(dir.filePath("note.md"), QString::fromUtf8("术语").toUtf8());
  SearchPanelWidget panel; panel.setRoot(dir.path());
  auto* field = panel.findChild<QLineEdit*>("WorkspaceSearchQuery");
  auto* tree = panel.findChild<QTreeWidget*>("WorkspaceSearchResults");
  field->setText(QString::fromUtf8("术语")); panel.startSearch({});
  field->setText("nothing"); panel.startSearch({});
  QElapsedTimer timer; timer.start();
  while (timer.elapsed() < 300) { QApplication::processEvents(); QThread::msleep(5); }
  require(tree->topLevelItemCount() == 0 && panel.result().matches == 0, "late old query does not pollute new results");
}
void narrowMatchPreview() {
  QTemporaryDir directory;
  write(directory.filePath("note.md"), QByteArray(100, 'a') + "needle" + QByteArray(100, 'b'));
  SearchPanelWidget panel; panel.setRoot(directory.path());
  panel.findChild<QLineEdit*>("WorkspaceSearchQuery")->setText("needle"); panel.startSearch({});
  auto* tree = panel.findChild<QTreeWidget*>("WorkspaceSearchResults");
  QElapsedTimer timer; timer.start();
  while (panel.result().matches != 1 && timer.elapsed() < 5000) { QApplication::processEvents(); QThread::msleep(5); }
  require(tree->topLevelItemCount() == 1, "long line match is found");
  require(!tree->topLevelItem(0)->text(0).contains('\n'), "root result does not repeat its filename as a path");
  QImage image(230, 28, QImage::Format_ARGB32); image.fill(Qt::white);
  QPainter painter(&image);
  QStyleOptionViewItem option; option.initFrom(tree); option.rect = image.rect(); option.widget = tree;
  const auto group = tree->model()->index(0, 0);
  tree->itemDelegate()->paint(&painter, option, tree->model()->index(0, 0, group)); painter.end();
  bool visibleMatch = false;
  for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
    if (image.pixelColor(x, y) == tree->palette().color(QPalette::Link)) visibleMatch = true;
  }
  require(visibleMatch, "narrow sidebar elides context before the hit so its highlight remains visible");
}
}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  runTest("workspace contents", search);
  runTest("preview write and durable recovery", replacementAndRecovery);
  runTest("encodings and regex captures", encodingAndCaptures);
  runTest("query generations", panelGeneration);
  runTest("narrow match context", narrowMatchPreview);
}
