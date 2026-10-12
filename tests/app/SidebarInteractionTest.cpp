#include "app/SidebarWidget.h"

#include "../TestUtils.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileSystemModel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTemporaryDir>
#include <QThread>
#include <QTreeView>

using namespace muffin;

namespace {

void waitUntil(const std::function<bool()>& ready, const char* message) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 15000) {
    QApplication::processEvents();
    QThread::msleep(5);
  }
  require(ready(), message);
}

void mouseEvent(QWidget* target, QEvent::Type type, const QPoint& pos,
                Qt::MouseButton button = Qt::LeftButton, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  QMouseEvent event(type, QPointF(pos), QPointF(target->mapToGlobal(pos)), button,
                    type == QEvent::MouseButtonRelease ? Qt::NoButton : button, modifiers);
  QApplication::sendEvent(target, &event);
}

void click(QWidget* target, const QPoint& pos, Qt::MouseButton button = Qt::LeftButton,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
  mouseEvent(target, QEvent::MouseButtonPress, pos, button, modifiers);
  mouseEvent(target, QEvent::MouseButtonRelease, pos, button, modifiers);
}

void doubleClick(QWidget* target, const QPoint& pos) {
  click(target, pos);
  mouseEvent(target, QEvent::MouseButtonDblClick, pos);
  mouseEvent(target, QEvent::MouseButtonRelease, pos);
}

struct FileTreeFixture {
  QTemporaryDir dir;
  SidebarWidget sidebar;
  QTreeView* tree = nullptr;
  QFileSystemModel* model = nullptr;
  QModelIndex folder;
  int openedFiles = 0;
  QString openedPath;

  FileTreeFixture() {
    require(dir.isValid(), "temporary folder should exist");
    require(QDir(dir.path()).mkpath(QStringLiteral("Folder/Nested")), "nested folders should exist");
    QFile file(dir.filePath(QStringLiteral("document.md")));
    require(file.open(QIODevice::WriteOnly), "document fixture should open");
    file.write("# Document\n");
    file.close();
    sidebar.resize(320, 600);
    sidebar.setFolderRoot(dir.path());
    sidebar.show();
    tree = sidebar.findChild<QTreeView*>(QStringLiteral("FileTree"));
    require(tree != nullptr, "file tree should exist");
    model = qobject_cast<QFileSystemModel*>(tree->model());
    require(model != nullptr, "file tree should use a filesystem model");
    waitUntil([&] {
      folder = model->index(dir.filePath(QStringLiteral("Folder")));
      return model->rowCount(tree->rootIndex()) == 2 && folder.isValid() && !tree->visualRect(folder).isEmpty();
    }, "file tree should load its root rows");
    QObject::connect(&sidebar, &SidebarWidget::fileOpenRequested, &sidebar, [this](const QString& path) {
      ++openedFiles;
      openedPath = path;
    });
  }

  QPoint folderLabel() const {
    const QRect row = tree->visualRect(folder);
    return QPoint(row.left() + 40, row.center().y());
  }
};

void testFolderNameAndRowToggleOnSingleClick() {
  FileTreeFixture f;
  require(!f.tree->isExpanded(f.folder), "folder should start collapsed");
  click(f.tree->viewport(), f.folderLabel());
  require(f.tree->isExpanded(f.folder), "single-clicking the folder name should expand it");
  require(f.tree->currentIndex() == f.folder, "folder click should also select the row");
  const QPoint blank(f.tree->viewport()->width() - 8, f.tree->visualRect(f.folder).center().y());
  require(f.tree->indexAt(blank) == f.folder, "blank space at the right should belong to the folder row");
  click(f.tree->viewport(), blank);
  require(!f.tree->isExpanded(f.folder), "clicking the blank part of the folder row should collapse it");
  require(f.openedFiles == 0, "folder clicks must not open a document");
}

void testFolderDoubleClickTogglesOnlyOnce() {
  FileTreeFixture f;
  doubleClick(f.tree->viewport(), f.folderLabel());
  require(f.tree->isExpanded(f.folder), "folder double-click must expand once without immediately collapsing");
  doubleClick(f.tree->viewport(), f.folderLabel());
  require(!f.tree->isExpanded(f.folder), "folder double-click must collapse once without reopening");
  require(f.openedFiles == 0, "folder double-clicks must not open a document");
}

void testArrowAndNestedFolderClicks() {
  FileTreeFixture f;
  const QRect row = f.tree->visualRect(f.folder);
  const QPoint arrow(row.left() - f.tree->indentation() / 2, row.center().y());
  click(f.tree->viewport(), arrow);
  require(f.tree->isExpanded(f.folder), "arrow click must expand exactly once");
  waitUntil([&] { return f.model->rowCount(f.folder) == 1; }, "expanded folder should load its child");
  const QModelIndex nested = f.model->index(f.dir.filePath(QStringLiteral("Folder/Nested")));
  const QRect nestedRow = f.tree->visualRect(nested);
  require(!nestedRow.isEmpty(), "nested folder row should be visible");
  click(f.tree->viewport(), QPoint(nestedRow.left() + 40, nestedRow.center().y()));
  require(f.tree->isExpanded(nested), "nested folder name should expand too");
  click(f.tree->viewport(), arrow);
  require(!f.tree->isExpanded(f.folder), "arrow click must collapse exactly once");
}

void testFileDoubleClickStillOpensDocument() {
  FileTreeFixture f;
  const QModelIndex file = f.model->index(f.dir.filePath(QStringLiteral("document.md")));
  const QRect row = f.tree->visualRect(file);
  const QPoint pos(row.left() + 40, row.center().y());
  click(f.tree->viewport(), pos);
  require(f.openedFiles == 0, "single-clicking a file should only select it");
  doubleClick(f.tree->viewport(), pos);
  require(f.openedFiles == 1, "double-clicking a file should open it once");
  require(f.openedPath == f.dir.filePath(QStringLiteral("document.md")), "file open should target the clicked file");
}

void testRightClickAndInlineRenameDoNotToggleFolder() {
  FileTreeFixture f;
  click(f.tree->viewport(), f.folderLabel(), Qt::RightButton);
  require(!f.tree->isExpanded(f.folder), "right-click should leave folder expansion unchanged");
  f.sidebar.beginInlineRename(f.dir.filePath(QStringLiteral("Folder")));
  QLineEdit* editor = nullptr;
  waitUntil([&] {
    editor = f.tree->viewport()->findChild<QLineEdit*>();
    return editor && editor->isVisible();
  }, "inline rename editor should open");
  click(editor, editor->rect().center());
  require(!f.tree->isExpanded(f.folder), "clicking inside the rename editor must not toggle its folder");
  QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
  QApplication::sendEvent(editor, &escape);
  require(f.openedFiles == 0, "renaming a folder must not open a document");
}

}  // namespace

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_MACOS
    qputenv("QT_QPA_PLATFORM", "cocoa");
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
  }
  QApplication app(argc, argv);
  runTest("testFolderNameAndRowToggleOnSingleClick", testFolderNameAndRowToggleOnSingleClick);
  runTest("testFolderDoubleClickTogglesOnlyOnce", testFolderDoubleClickTogglesOnlyOnce);
  runTest("testArrowAndNestedFolderClicks", testArrowAndNestedFolderClicks);
  runTest("testFileDoubleClickStillOpensDocument", testFileDoubleClickStillOpensDocument);
  runTest("testRightClickAndInlineRenameDoNotToggleFolder", testRightClickAndInlineRenameDoNotToggleFolder);
  return 0;
}
