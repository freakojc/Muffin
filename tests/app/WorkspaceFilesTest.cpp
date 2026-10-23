#include "app/WorkspaceFiles.h"
#include "app/QuickOpenDialog.h"
#include "theme/ThemeDefinition.h"
#include "../TestUtils.h"
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QImage>
#include <QStyleFactory>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTemporaryDir>
#include <QThread>

using namespace muffin;
namespace {
void write(const QString& path) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "create fixture");
  file.write("note");
}
void waitUntil(const std::function<bool()>& ready) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 10000) {
    QApplication::processEvents();
    QThread::msleep(5);
  }
  require(ready(), "asynchronous picker should finish");
}
void matching() {
  const QString path = QString::fromUtf8("04.学习/英语/单词.md");
  require(PathMatcher::score(QString::fromUtf8("学习 单词"), path) >= 0, "AND terms across path");
  require(PathMatcher::score(QString::fromUtf8("04/单词"), path) >= 0, "skip intermediate directory");
  require(PathMatcher::score(QString::fromUtf8("单词/04"), path) < 0, "path segments stay ordered");
  require(PathMatcher::score(QString::fromUtf8("随记 单词"), path) < 0, "every term must match");
  require(PathMatcher::score(QString::fromUtf8("朝路"), QString::fromUtf8("阅读/朝圣之路.md")) >= 0,
          "Chinese subsequence");
  require(PathMatcher::score("readme", "README.md") > PathMatcher::score("readme", "readme/notes.md"),
          "filename ranks above directory");
  require(PathMatcher::score("ab/c", "axb/ab/c.md") >= 0, "best segment alignment");
  require(PathMatcher::score("foo\\note", "foo/deep/note.md") >= 0, "Windows query separators");
}
void scanAndPicker() {
  QTemporaryDir directory, outside;
  const auto a = directory.filePath(QString::fromUtf8("04.学习/英语/单词.md"));
  const auto b = directory.filePath(QString::fromUtf8("06.随记/英语/单词.md"));
  write(a);
  write(b);
  write(directory.filePath("build/ignored.md"));
  write(directory.filePath("node_modules/ignored.md"));
  write(directory.filePath("image.png"));
  write(outside.filePath("other.md"));
  auto cancel = std::make_shared<std::atomic_bool>(false);
  const auto files = WorkspaceFileIndex::scan(directory.path(), {"md"}, cancel);
  require(files.paths.size() == 2, "scan deep files, exclude cache and non-text types");
  require(!WorkspaceFileIndex::contains(directory.path(), outside.filePath("other.md")), "root boundary");
  require(WorkspaceFileIndex::scan(directory.path(), {"md"}, cancel, 1).truncated,
          "limited index is explicitly incomplete");
  cancel->store(true);
  require(WorkspaceFileIndex::scan(directory.path(), {"md"}, cancel).paths.isEmpty(), "cancel scan");
  QuickOpenDialog dialog;
  dialog.setCandidates({outside.filePath("other.md")});
  dialog.setWorkspace(directory.path(), {"md"});
  auto* list = dialog.findChild<QListWidget*>("QuickOpenResults");
  auto* filter = dialog.findChild<QLineEdit*>("QuickOpenFilter");
  waitUntil([&] { return list->count() == 2; });
  require(list->item(0)->data(Qt::UserRole + 1) != list->item(1)->data(Qt::UserRole + 1),
          "same filename shows distinct paths");
  filter->setText(QString::fromUtf8("04/单词"));
  waitUntil([&] { return list->count() == 1; });
  require(dialog.selectedPath() == a, "picker searches full workspace, excludes outside history");
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(filter, &enter);
  require(dialog.result() == QDialog::Accepted, "Enter opens matching file");
}
void pickerRanking() {
  QTemporaryDir directory;
  const auto weak = directory.filePath("alpha/notes.md"), strong = directory.filePath("alpha.md");
  write(weak);
  write(strong);
  QuickOpenDialog dialog;
  dialog.setCandidates({weak, strong});
  auto* filter = dialog.findChild<QLineEdit*>("QuickOpenFilter");
  filter->setText("alpha");
  QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
  QApplication::sendEvent(filter, &enter);
  require(dialog.selectedPath() == strong,
          "a new query selects the highest ranked match rather than the previous selection");
}
void pickerPresentation() {
  QTemporaryDir directory;
  const auto rootFile = directory.filePath("note.md"), nested = directory.filePath("deep/note.md");
  write(rootFile);
  write(nested);
  for (const auto& style : QStyleFactory::keys()) {
    QApplication::setStyle(QStyleFactory::create(style));
    QuickOpenDialog dialog;
    dialog.setWorkspace(directory.path(), {"md"});
    auto* list = dialog.findChild<QListWidget*>("QuickOpenResults");
    auto* filter = dialog.findChild<QLineEdit*>("QuickOpenFilter");
    waitUntil([&] { return list->count() == 2; });
    require(list->item(0)->data(Qt::UserRole + 1).toString().isEmpty(), "root file name is displayed once");
    require(list->item(1)->data(Qt::UserRole + 1).toString() == "deep", "secondary row shows only the parent path");
    filter->setText("note");
    filter->setCursorPosition(4);
    QKeyEvent home(QEvent::KeyPress, Qt::Key_Home, Qt::NoModifier);
    QApplication::sendEvent(filter, &home);
    require(filter->cursorPosition() == 0, "Home edits the filter rather than moving the result selection");
    for (const auto& theme : {QStringLiteral("github"), QStringLiteral("night")}) {
      const auto definition = ThemeDefinition::builtIn(theme).value();
      dialog.setThemeDefinition(definition);
      dialog.ensurePolished();
      list->ensurePolished();
      QImage image(400, 40, QImage::Format_ARGB32);
      image.fill(definition.colors.surface);
      QPainter painter(&image);
      QStyleOptionViewItem option;
      option.initFrom(list);
      option.rect = image.rect();
      option.widget = list;
      option.state |= QStyle::State_Selected;
      list->itemDelegate()->paint(&painter, option, list->model()->index(0, 0));
      painter.end();
      require(image.pixelColor(380, 20) == definition.colors.chromeSelection,
              QStringLiteral("%1/%2: selected fill %3, expected %4 (view palette %5)")
                  .arg(style, theme, image.pixelColor(380, 20).name(), definition.colors.chromeSelection.name(),
                       list->palette().color(QPalette::Highlight).name()));
      bool visibleInk = false;
      for (int y = 4; y < 34; ++y)
        for (int x = 10; x < 120; ++x) {
          if (image.pixelColor(x, y) == definition.colors.chromeSelectionText)
            visibleInk = true;
        }
      require(visibleInk, "selected filename uses the foreground paired with its actual background");
      if (const auto screenshots = qEnvironmentVariable("MUFFIN_SEARCH_SCREENSHOT_DIR"); !screenshots.isEmpty()) {
        QDir().mkpath(screenshots);
        dialog.grab().save(QDir(screenshots).filePath("quick-open-" + style + '-' + theme + ".png"));
      }
    }
  }
}
}
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  runTest("path matching", matching);
  runTest("recursive scan and picker", scanAndPicker);
  runTest("picker ranking", pickerRanking);
  runTest("picker native selection and text editing", pickerPresentation);
}
