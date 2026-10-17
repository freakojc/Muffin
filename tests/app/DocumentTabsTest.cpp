#include "app/MainWindow.h"
#include "app/LanguageManager.h"
#include "app/FileTreeModel.h"
#include "editor/EditorView.h"
#include "editor/SourceEditorWidget.h"
#include "editor/VirtualSourceEdit.h"
#include "render/BlockLayout.h"
#include "render/ImageLoader.h"
#include "render/LayoutResources.h"

#include "../TestUtils.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QMessageBox>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QtTest/QTest>

using namespace muffin;

namespace {

void waitUntil(const std::function<bool()>& ready, const char* message) {
  QElapsedTimer timer;
  timer.start();
  while (!ready() && timer.elapsed() < 15000) {
    QApplication::processEvents();
    QTest::qWait(5);
  }
  require(ready(), message);
}

QAction* action(MainWindow& window, const QString& text) {
  for (QAction* candidate : window.findChildren<QAction*>()) {
    if (candidate->text() == text) { return candidate; }
  }
  require(false, QStringLiteral("Missing action: %1").arg(text));
  return nullptr;
}

struct Fixture {
  QTemporaryDir directory;
  SettingsOverride sourceMode{"view/sourceMode", false};
  MainWindow window;
  QTabWidget* tabs = nullptr;

  Fixture() {
    require(directory.isValid(), "temporary directory");
    tabs = window.findChild<QTabWidget*>(QStringLiteral("DocumentTabs"));
    require(tabs != nullptr, "document tabs exist");
    window.resize(1000, 700);
    window.show();
    QApplication::processEvents();
  }

  ~Fixture() {
    // Tests own their dirty buffers and recovery snapshots; do not leave drafts behind.
    DraftRecovery recovery;
    for (int i = 0; i < tabs->count(); ++i) {
      auto* document = static_cast<DocumentTab*>(tabs->widget(i));
      document->session.document().setModified(false);
      recovery.markClean(document->session.filePath(), document->draftKey);
    }
  }

  DocumentTab* current() { return static_cast<DocumentTab*>(tabs->currentWidget()); }

  QString file(const QString& name, const QString& text) {
    const QString path = directory.filePath(name);
    QFile out(path);
    require(out.open(QIODevice::WriteOnly), "create test file");
    require(out.write(text.toUtf8()) > 0, "write test file");
    return path;
  }

  DocumentTab* open(const QString& path) {
    require(window.openFile(path), "open file");
    DocumentTab* document = current();
    waitUntil([document] { return !document->session.isAsyncParseInProgress(); }, "file finished loading");
    QApplication::processEvents(); // drain queued path/base updates before editing or scrolling
    return document;
  }

  void close(DocumentTab* document) { tabs->tabCloseRequested(tabs->indexOf(document)); }
};

// Interact with the real modal prompt while its nested event loop is running.
class AnswerPrompts {
public:
  QTimer timer;
  int count = 0;
  explicit AnswerPrompts(QMessageBox::StandardButton answer) {
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, &timer, [this, answer] {
      if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
        ++count;
        require(box->button(answer) != nullptr, "expected modal button");
        box->button(answer)->click();
      }
      require(count < 10, "unexpected repeated prompts");
    });
    timer.start();
  }
};

QString contents(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "read saved document");
  return QString::fromUtf8(file.readAll()).replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
}

void closeFolder(MainWindow& window) {
  auto* sidebar = window.findChild<SidebarWidget*>();
  auto* tree = sidebar->findChild<QTreeView*>("FileTree");
  bool triggered = false;
  QTimer::singleShot(0, &window, [&] {
    auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
    require(menu != nullptr, "file tree context menu opens");
    QAction* close = nullptr;
    bool collapse = false;
    for (auto* item : menu->actions()) {
      require(item->text() != "Expand All Folders", "context menu omits recursive expand-all");
      if (item->text() == "Close Folder") close = item;
      if (item->text() == "Collapse All Folders") collapse = true;
    }
    require(close && collapse, "context menu retains close-folder and collapse-all actions");
    close->trigger();
    triggered = true;
    menu->close();
  });
  sidebar->fileTreeContextMenuRequested(sidebar->folderRoot(), true, false, tree->mapToGlobal(tree->rect().center()));
  require(triggered, "closing the workspace remains accessible from the context menu");
}

void tabBarOnlyForMultipleDocuments() {
  Fixture f;
  auto* bar = f.tabs->tabBar();
  auto* plus = f.window.findChild<QToolButton*>(QStringLiteral("NewTabButton"));
  auto* sidebar = f.window.findChild<SidebarWidget*>();
  auto checkSingle = [&] {
    QApplication::processEvents();
    require(f.tabs->count() == 1 && !bar->isVisible() && !plus->isVisible(),
            "single document hides tab bar and new-tab button");
    require(f.current()->mapTo(f.tabs, QPoint(0, 0)).y() <= 1,
            "hidden tab strip leaves no empty row above the editor");
  };
  checkSingle();
  const QString first = f.file(QStringLiteral("README.md"), QStringLiteral("# Single document\n\nWrite without a tab strip.\n"));
  const QString second = f.file(QStringLiteral("example.md"), QStringLiteral("# Second document\n"));
  sidebar->fileOpenRequested(first);
  DocumentTab* a = f.current();
  checkSingle();
  require(a->session.filePath() == first, "sidebar opens the first document");
  if (const QString screenshot = qEnvironmentVariable("MUFFIN_TAB_SCREENSHOT"); !screenshot.isEmpty()) {
    require(f.window.grab().save(screenshot + QStringLiteral(".single.png")), "save single-document appearance capture");
  }
  sidebar->fileOpenRequested(second);
  DocumentTab* b = f.current();
  QApplication::processEvents();
  require(a != b && f.tabs->count() == 2 && bar->isVisible() && plus->isVisible(),
          "opening the second sidebar document shows the tab strip");
  require(b->mapTo(f.tabs, QPoint(0, 0)).y() >= bar->height(), "visible tab strip sits above the editor");
  sidebar->fileOpenRequested(first);
  require(f.tabs->count() == 2 && f.current() == a, "reopening a sidebar document selects its tab");
  f.close(b);
  checkSingle();
  sidebar->fileOpenRequested(second);
  QApplication::processEvents();
  require(bar->isVisible() && plus->isVisible(), "tab strip reappears when a second document is reopened");
  f.close(f.current());
  checkSingle();
  {
    AnswerPrompts answer(QMessageBox::Ok);
    require(!f.window.openFile(f.directory.filePath(QStringLiteral("missing.md"))), "failed second open reports failure");
    checkSingle();
  }
}

void formatActionsFollowSelectionAndDocumentChanges() {
  Fixture f;
  auto* document = f.current();
  const QString text = QStringLiteral("**bold** *italic* <u>under</u> `code` $x$ ~~strike~~ plain\n");
  document->session.setMarkdownText(text, true);
  auto* paragraph = document->session.document().root().children().front().get();
  const QStringList names{"Bold", "Italic", "Underline", "Inline Code", "Inline Formula", "Strikethrough"};
  const QStringList tokens{"bold", "italic", "under", "code", "x", "strike", "plain"};
  auto at = [&](const QString& token) {
    CursorPosition position;
    position.blockId = position.text.nodeId = paragraph->id();
    position.text.sourceOffset = text.indexOf(token) + 1;
    position.text.textOffset = position.text.sourceOffset;
    document->editorController.selection().setCursorPosition(position);
    QApplication::processEvents();
  };
  for (int i = 0; i < tokens.size(); ++i) {
    at(tokens[i]);
    const auto expected = document->renderCommands.currentInlineFormats();
    const bool checked[]{expected.bold, expected.italic, expected.underline, expected.code, expected.inlineMath, expected.strikethrough};
    for (int j = 0; j < names.size(); ++j)
      require(action(f.window, names[j])->isChecked() == checked[j], "format menu must match current selection snapshot");
  }
  at("bold");
  auto* second = f.open(f.file("plain.md", "plain second document\n"));
  CursorPosition plain;
  plain.blockId = plain.text.nodeId = second->session.document().root().children().front()->id();
  second->editorController.selection().setCursorPosition(plain);
  QApplication::processEvents();
  for (const auto& name : names) require(!action(f.window, name)->isChecked(), "format snapshot must not leak across tabs");
  f.tabs->setCurrentWidget(document);
  QApplication::processEvents();
  require(action(f.window, "Bold")->isChecked(), "reactivating tab refreshes formatting");
}

void sharedCursorQueriesMatchIndependentQueries() {
  Fixture f;
  auto* document = f.current();
  const QStringList sources{
      "**bold** *italic* <u>under</u> `code` $x$ ~~strike~~ plain\n",
      "prefix ![alt](image.png) and <img src=\"other.png\"> suffix\n",
      "<img src=\"standalone.png\">\n",
      "- **nested** ![alt](list.png)\n",
      "| A | B |\n|---|---|\n| **bold** | ![alt](cell.png) |\n",
      "```cpp\ntext\n```\n"};
  bool sawImage = false, sawBold = false;
  for (const auto& source : sources) {
    document->session.setMarkdownText(source, true);
    document->editorController.selection().clear();
    require(document->renderCommands.currentCursorState().imageSource.isEmpty(), "empty cursor has no image");
    const auto& children = document->session.document().root().children();
    require(!children.empty(), "cursor query fixture has a block");
    auto* block = children.front().get();
    for (qsizetype offset = 0; offset < source.size(); ++offset) {
      CursorPosition cursor;
      cursor.blockId = cursor.text.nodeId = block->id();
      cursor.text.sourceOffset = offset;
      cursor.text.textOffset = offset;
      document->editorController.selection().setCursorPosition(cursor);
      const auto shared = document->renderCommands.currentCursorState();
      sawImage |= !shared.imageSource.isEmpty();
      sawBold |= shared.formats.bold;
      const auto formats = document->renderCommands.currentInlineFormats();
      require(shared.formats.bold == formats.bold && shared.formats.italic == formats.italic &&
                  shared.formats.underline == formats.underline && shared.formats.code == formats.code &&
                  shared.formats.strikethrough == formats.strikethrough && shared.formats.inlineMath == formats.inlineMath,
              "shared formats match individual query at each source position");
      require(shared.imageSource == document->renderCommands.imageSrcAtCursor(), "shared image query matches individual query");
    }
    auto range = document->editorController.selection().selection();
    range.anchor.text.textOffset = 0;
    range.anchor.text.sourceOffset = 0;
    document->editorController.selection().setSelection(range);
    const auto shared = document->renderCommands.currentCursorState();
    const auto formats = document->renderCommands.currentInlineFormats();
    require(shared.formats.bold == formats.bold && shared.formats.italic == formats.italic &&
                shared.formats.underline == formats.underline && shared.formats.code == formats.code &&
                shared.formats.strikethrough == formats.strikethrough && shared.formats.inlineMath == formats.inlineMath,
            "selection queries retain independent format semantics");
  }
  require(sawImage && sawBold, "query fixtures exercise images and formatted spans");
}

void retainsDocumentsAndRenderState() {
  Fixture f;
  const QString original = QStringLiteral("# First\n\n") + QStringLiteral("A paragraph of text.\n\n").repeated(180);
  const QString first = f.file(QStringLiteral("first.md"), original);
  const QString second = f.file(QStringLiteral("second.md"), QStringLiteral("# Second\n\nOther content.\n"));
  DocumentTab* a = f.open(first);
  require(f.tabs->count() == 1, "pristine startup tab reused");
  a->editorController.applyMarkdownTextWithUndo(original + QStringLiteral("Edited A.\n"), QStringLiteral("edit"));
  require(a->editorController.moveDocumentEnd(), "move caret to end");
  QApplication::processEvents();
  a->renderView->verticalScrollBar()->setValue(a->renderView->verticalScrollBar()->maximum() / 2);
  const int scroll = a->renderView->verticalScrollBar()->value();
  const qsizetype caret = a->editorController.selection().cursorPosition().text.sourceOffset;
  require(scroll > 0, "nonzero render scroll");
  require(f.tabs->tabText(0).endsWith(QStringLiteral(" *")), "dirty marker on tab");

  DocumentTab* b = f.open(second);
  require(f.tabs->count() == 2 && a != b, "opening a file adds a document");
  require(!b->editorController.canUndo(), "new file has independent undo history");
  require(a->session.markdownText().toString().endsWith(QStringLiteral("Edited A.\n")), "opening B retains unsaved A");
  require(f.window.openFile(QFileInfo(first).absolutePath() + QStringLiteral("/./first.md")), "reopen equivalent path");
  require(f.tabs->count() == 2 && f.current() == a, "equivalent path activates existing tab");
  QApplication::processEvents();
  require(a->renderView->verticalScrollBar()->value() == scroll, "switch preserves render scroll");
  require(a->editorController.selection().cursorPosition().text.sourceOffset == caret, "switch preserves render caret");
  action(f.window, QStringLiteral("Undo"))->trigger();
  require(a->session.markdownText().toString() == original, "undo still belongs to A");
  require(b->session.markdownText().toString().contains(QStringLiteral("Second")), "undo leaves B intact");
  f.tabs->tabBar()->moveTab(0, 1);
  require(f.window.openFile(first) && f.current() == a, "reordered tabs still resolve by document");
}

void sourceStateAndKeyboardNavigation() {
  Fixture f;
  DocumentTab* a = f.open(f.file(QStringLiteral("a.md"), QStringLiteral("Source A\n\n").repeated(150)));
  action(f.window, QStringLiteral("Source Code Mode"))->trigger();
  require(a->backend->isSourceMode(), "switch A to source mode");
  a->editor->setCursorPosition(450);
  a->editor->insertText(QStringLiteral("edited"));
  auto* source = a->editor->findChild<VirtualSourceEdit*>();
  require(source != nullptr, "source canvas");
  source->verticalScrollBar()->setValue(source->verticalScrollBar()->maximum() / 2);
  const int scroll = source->verticalScrollBar()->value();
  const qsizetype caret = a->editor->cursorPosition();
  DocumentTab* b = f.open(f.file(QStringLiteral("b.md"), QStringLiteral("Source B\n")));
  require(b->backend->isSourceMode(), "new tab inherits current view mode");
  b->editor->insertText(QStringLiteral("B edit"));
  b->viewStack->currentWidget()->setFocus();
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_Tab, Qt::ControlModifier);
  require(f.current() == a, "Ctrl+Tab switches from source canvas");
  require(a->editor->cursorPosition() == caret && source->verticalScrollBar()->value() == scroll,
          "source caret and scroll retained");
  action(f.window, QStringLiteral("Undo"))->trigger();
  require(!a->session.markdownText().toString().contains(QStringLiteral("edited")), "source undo retained");
  require(b->session.markdownText().toString().contains(QStringLiteral("B edit")), "source undo isolated");
  action(f.window, QStringLiteral("Source Code Mode"))->trigger();
  require(!a->backend->isSourceMode(), "A returns to render mode");
  f.tabs->setCurrentWidget(b);
  require(action(f.window, QStringLiteral("Source Code Mode"))->isChecked(), "mode action follows active tab");
  f.tabs->setCurrentWidget(a);
  a->renderView->setFocus();
  QTest::keyClick(a->renderView, Qt::Key_Tab, Qt::ControlModifier);
  require(f.current() == b, "Ctrl+Tab switches from render canvas");
  QTest::keyClick(QApplication::focusWidget(), Qt::Key_N, Qt::ControlModifier);
  require(f.tabs->count() == 3 && f.current()->session.filePath().isEmpty(), "Ctrl+N creates an untitled tab");
  f.current()->editor->insertText(QStringLiteral("Untitled edits"));
  f.open(f.file(QStringLiteral("c.md"), QStringLiteral("C\n")));
  require(f.tabs->count() == 4, "edited untitled tab is retained when opening a file");
  action(f.window, QStringLiteral("New"))->trigger();
  DocumentTab* empty = f.current();
  empty->editor->insertText(QStringLiteral("Undoable draft"));
  empty->editor->undo();
  require(empty->session.markdownText().isEmpty() && empty->editor->canRedo(), "empty draft retains redo history");
  f.open(f.file(QStringLiteral("d.md"), QStringLiteral("D\n")));
  require(f.tabs->count() == 6 && f.tabs->indexOf(empty) >= 0, "opening a file preserves an empty draft's redo history");
}

void closesWithSaveDiscardAndCancel() {
  Fixture f;
  const QString path = f.file(QStringLiteral("a.md"), QStringLiteral("A\n"));
  DocumentTab* a = f.open(path);
  a->editorController.applyMarkdownTextWithUndo(QStringLiteral("Edited A\n"), QStringLiteral("edit"));
  DocumentTab* b = f.open(f.file(QStringLiteral("b.md"), QStringLiteral("B\n")));
  {
    AnswerPrompts answer(QMessageBox::Cancel);
    f.close(a);
    require(answer.count == 1 && f.tabs->count() == 2 && f.current() == b,
            "cancel background close preserves tabs and active document");
  }
  {
    AnswerPrompts answer(QMessageBox::Save);
    f.close(a);
    require(answer.count == 1 && f.tabs->count() == 1 && f.current() == b, "save closes just A");
    require(contents(path) == QStringLiteral("Edited A\n"), "close saves the correct document");
  }
  b->editorController.applyMarkdownTextWithUndo(QStringLiteral("Unsaved B\n"), QStringLiteral("edit"));
  {
    AnswerPrompts answer(QMessageBox::Discard);
    f.close(b);
    require(answer.count == 1 && f.tabs->count() == 1 && f.current()->session.filePath().isEmpty(),
            "closing last tab leaves a fresh untitled page");
  }
  auto* newButton = f.window.findChild<QToolButton*>(QStringLiteral("NewTabButton"));
  f.window.activateWindow();
  f.current()->renderView->setFocus();
  QApplication::processEvents();
  QTest::keyClick(f.current()->renderView, Qt::Key_N, Qt::ControlModifier);
  QApplication::processEvents();
  require(f.tabs->count() == 2, "Ctrl+N opens a tab while the tab strip is hidden");
  require(newButton->isVisible(), "new-tab button appears after creating a second tab");
  QTest::mouseClick(newButton, Qt::LeftButton);
  require(f.tabs->count() == 3, "visible plus opens a tab");
  auto* bar = f.tabs->tabBar();
  QTest::mouseClick(bar, Qt::MiddleButton, Qt::NoModifier, bar->tabRect(0).center());
  require(f.tabs->count() == 2, "middle click closes a tab");
  action(f.window, QStringLiteral("Close Tab"))->trigger();
  require(f.tabs->count() == 1 && !bar->isVisible(), "closing down to one document hides the tab strip");
  action(f.window, QStringLiteral("Close Tab"))->trigger();
  require(f.tabs->count() == 1 && f.window.isVisible(), "Ctrl+W action closes tab and keeps window usable");
}

void backgroundLoadingAndPersistence() {
  Fixture f;
  const QString first = f.file(QStringLiteral("large.md"), QStringLiteral("# Large\n\n") + QStringLiteral("Long paragraph.\n\n").repeated(12000));
  const QString second = f.file(QStringLiteral("small.md"), QStringLiteral("# Small\n\nSmall body.\n"));
  require(f.window.openFile(first), "start async large open");
  DocumentTab* a = f.current();
  require(a->session.isAsyncParseInProgress(), "large document still loading");
  require(f.window.openFile(second), "open second while first is parsing");
  DocumentTab* b = f.current();
  waitUntil([a, b] { return !a->session.isAsyncParseInProgress() && !b->session.isAsyncParseInProgress(); }, "both parses finish");
  require(f.current() == b && f.window.windowTitle().startsWith(QStringLiteral("small.md")), "background parse leaves active title intact");
  require(a->session.markdownText().toString().contains(QStringLiteral("Large")) && b->editor->text().contains(QStringLiteral("Small")),
          "parses update the owning documents");
  a->session.setMarkdownText(QStringLiteral("Dirty A\n"), true);
  b->session.setMarkdownText(QStringLiteral("Dirty B\n"), true);
  QMetaObject::invokeMethod(a->draftTimer, "timeout", Qt::DirectConnection);
  QMetaObject::invokeMethod(b->draftTimer, "timeout", Qt::DirectConnection);
  DraftRecovery recovery;
  waitUntil([&] { return a->lastDraftSnapshotRevision == a->session.document().revision() &&
                         b->lastDraftSnapshotRevision == b->session.document().revision(); }, "background drafts committed");
  const auto drafts = recovery.pendingDrafts();
  require(drafts.size() == 2 && a->draftKey != b->draftKey, "each tab has its own recovery snapshot");
  {
    SettingsOverride autoSave("files/autoSave", true);
    QMetaObject::invokeMethod(a->autoSaveTimer, "timeout", Qt::DirectConnection);
    require(contents(first) == QStringLiteral("Dirty A\n"), "background autosave writes A");
    require(b->session.document().isModified() && contents(second).contains(QStringLiteral("Small")), "autosave leaves B unsaved");
    waitUntil([&] { return recovery.pendingDrafts().size() == 1; }, "saving A leaves B recovery snapshot");
  }
  a->session.setMarkdownText(QStringLiteral("Save All A\n"), true);
  action(f.window, QStringLiteral("Save All Open Files..."))->trigger();
  require(contents(first) == QStringLiteral("Save All A\n") && contents(second) == QStringLiteral("Dirty B\n"), "Save All includes inactive tabs");
  require(f.current() == b, "Save All retains active tab");
  waitUntil([&] { return recovery.pendingDrafts().isEmpty(); }, "Save All clears saved drafts asynchronously");

  // Closing an in-flight parse must not leave a worker referring to a destroyed session.
  const QString closing = f.file(QStringLiteral("closing.md"), QStringLiteral("Closing worker.\n\n").repeated(20000));
  require(f.window.openFile(closing), "start parse to close");
  f.close(f.current());
  QThreadPool::globalInstance()->waitForDone();
  QApplication::processEvents();
  require(f.tabs->count() == 2, "async tab closed safely");

  a->session.setMarkdownText(QStringLiteral("Exit dirty A\n"), true);
  {
    AnswerPrompts answer(QMessageBox::Cancel);
    require(!f.window.close() && answer.count == 1 && f.window.isVisible(), "window close checks inactive dirty tabs");
  }
  {
    AnswerPrompts answer(QMessageBox::Discard);
    require(f.window.close() && answer.count == 1, "window close can discard remaining dirty tabs");
  }
}

void openFailuresAndConflictingSaveAs() {
  Fixture f;
  DocumentTab* a = f.open(f.file(QStringLiteral("a.md"), QStringLiteral("A\n")));
  const QString second = f.file(QStringLiteral("b.md"), QStringLiteral("B\n"));
  DocumentTab* b = f.open(second);
  a->session.setMarkdownText(QStringLiteral("Unsaved A\n"), true);
  f.tabs->setCurrentWidget(a);
  {
    AnswerPrompts answer(QMessageBox::Ok);
    require(!f.window.openFile(f.directory.filePath(QStringLiteral("missing.md"))), "failed open reports failure");
    require(answer.count == 1 && f.tabs->count() == 2 && f.current() == a, "failed open leaves no extra tab");
  }
  {
    QTimer timer;
    timer.setInterval(5);
    QObject::connect(&timer, &QTimer::timeout, &timer, [] {
      if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
        dialog->reject();
      }
    });
    timer.start();
    require(!f.window.openFile({}) && f.tabs->count() == 2 && f.current() == a, "cancel open leaves documents intact");
  }
  {
    AnswerPrompts answer(QMessageBox::Ok);
    QTimer choose;
    choose.setInterval(5);
    QObject::connect(&choose, &QTimer::timeout, &choose, [&] {
      if (auto* dialog = qobject_cast<QFileDialog*>(QApplication::activeModalWidget())) {
        dialog->setOption(QFileDialog::DontConfirmOverwrite, true);
        dialog->selectFile(second);
        QMetaObject::invokeMethod(dialog, "accept", Qt::DirectConnection);
        choose.stop();
      }
    });
    choose.start();
    action(f.window, QStringLiteral("Save As..."))->trigger();
    require(answer.count == 1 && a->session.document().isModified(),
            QStringLiteral("Save As refuses another open document (prompts=%1, modified=%2, path=%3)")
                .arg(answer.count).arg(a->session.document().isModified()).arg(a->session.filePath()));
    require(contents(second) == QStringLiteral("B\n") && b->session.markdownText().toString() == QStringLiteral("B\n"),
            "conflicting Save As preserves both disk and open B");
  }
  {
    SettingsOverride saveOnSwitch("files/autoSaveOnSwitch", true);
    f.tabs->setCurrentWidget(b);
    require(!a->session.document().isModified() && contents(a->session.filePath()) == QStringLiteral("Unsaved A\n"),
            "save-on-switch preference still applies to retained documents");
    b->session.setMarkdownText(QStringLiteral("Discard B\n"), true);
    AnswerPrompts answer(QMessageBox::Discard);
    f.close(b);
    require(answer.count == 1 && contents(second) == QStringLiteral("B\n"), "closing with Discard never saves on switch");
  }
}

void renameFolderAndTranslateTabs() {
  Fixture f;
  const QString folder = f.directory.filePath(QStringLiteral("folder"));
  require(QDir().mkpath(folder), "create folder for rename");
  DocumentTab* a = f.open(f.file(QStringLiteral("folder/a.md"), QStringLiteral("# First document\n\nEditing one document keeps the others open.\n")));
  DocumentTab* b = f.open(f.file(QStringLiteral("folder/b.md"), QStringLiteral("# Second document\n\nSwitch tabs without losing your place.\n")));
  a->session.setMarkdownText(QStringLiteral("# First document\n\nUnsaved changes in another tab.\n"), true);
  auto* sidebar = f.window.findChild<SidebarWidget*>();
  sidebar->inlineCommitRequested(InlineEditContext{folder, false, true}, QStringLiteral("renamed"));
  const QString renamed = f.directory.filePath(QStringLiteral("renamed"));
  require(a->session.filePath() == renamed + QStringLiteral("/a.md") && b->session.filePath() == renamed + QStringLiteral("/b.md"),
          "folder rename updates both active and inactive paths");
  require(f.window.saveCurrentDocument(), "renamed active file saves");
  require(f.window.openFile(a->session.filePath()) && f.tabs->count() == 2 && f.current() == a,
          "renamed inactive file reuses its tab");

  for (const LanguageInfo& language : LanguageManager::instance().availableLanguages()) {
    if (language.code == QStringLiteral("system")) { continue; }
    require(LanguageManager::instance().setLanguage(language.code), "tab language loads");
    const QString close = MainWindow::tr("Close Tab");
    require(!close.isEmpty(), "translated close label");
    if (language.code != QStringLiteral("en")) {
      require(close != QStringLiteral("Close Tab"), "tab controls translated in every locale");
    }
    auto* button = f.tabs->tabBar()->tabButton(0, QTabBar::RightSide);
    require(button->toolTip() == close && button->accessibleName() == close, "existing close button retranslates");
    auto* plus = f.window.findChild<QToolButton*>(QStringLiteral("NewTabButton"));
    require(plus->toolTip().startsWith(MainWindow::tr("New Tab")), "new-tab tooltip retranslates");
    require(f.tabs->tabBar()->accessibleName() == MainWindow::tr("Open Documents"), "tab bar accessible name retranslates");
  }
  LanguageManager::instance().setLanguage(QStringLiteral("en"));
  if (const QString screenshot = qEnvironmentVariable("MUFFIN_TAB_SCREENSHOT"); !screenshot.isEmpty()) {
    f.tabs->setCurrentWidget(b);
    QApplication::processEvents();
    require(f.window.grab().save(screenshot), "save tab appearance capture");
    action(f.window, QStringLiteral("Night"))->trigger();
    require(f.window.grab().save(screenshot + QStringLiteral(".night.png")), "save dark tab appearance capture");
  }
}

void hiddenTabsDeferThemeChanges() {
  Fixture f;
  auto* a = f.open(f.file("theme-a.md", "# A\n\ntext\n"));
  auto* b = f.open(f.file("theme-b.md", "# B\n\ntext\n"));
  const auto previous = a->renderView->theme().backgroundColor();
  const auto revision = a->session.document().revision();
  const auto cursor = a->renderView->cursorPosition();
  action(f.window, QStringLiteral("Night"))->trigger();
  require(a->pendingTheme.has_value() && !b->pendingTheme, "only hidden tab defers theme application");
  require(a->renderView->theme().backgroundColor() == previous, "hidden editor retains previous layout snapshot");
  const auto first = a->pendingTheme->backgroundColor();
  action(f.window, QStringLiteral("GitHub"))->trigger();
  require(a->pendingTheme && a->pendingTheme->backgroundColor() != first, "repeated changes coalesce to latest theme");
  const auto expected = b->renderView->theme().backgroundColor();
  f.tabs->setCurrentWidget(a);
  require(!a->pendingTheme && a->renderView->theme().backgroundColor() == expected, "activation applies latest snapshot once");
  require(a->session.document().revision() == revision && a->renderView->cursorPosition().text.nodeId == cursor.text.nodeId,
          "deferred theme preserves document and cursor identity");
}

void hiddenTabsMergeThemeAndResourceChanges() {
  Fixture f;
  auto& loader = ImageLoader::instance();
  auto& resources = LayoutResources::instance();
  const QString url = "https://example.invalid/tab-resource.png";
  const auto store = [&](int height) {
    QImage image(60, height, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::blue);
    loader.store(url, image);
  };
  store(30);
  auto* a = f.open(f.file(
      "resources-a.md", "![image](" + url + ")\n\n" + QString("Text with **bold** and wrapping words. ").repeated(30)));
  const auto imageId = a->session.document().root().children()[0]->id();
  const auto textId = a->session.document().root().children()[1]->id();
  CursorPosition cursor;
  cursor.blockId = cursor.text.nodeId = textId;
  cursor.text.textOffset = 5;
  cursor.text.sourceOffset = a->session.document().root().children()[1]->sourceRange().byteStart + 5;
  a->editorController.selection().setCursorPosition(cursor);
  auto* b = f.open(f.file("resources-b.md", "# Active\n\nsecond tab"));
  const auto oldHeight = a->renderView->nodeRect(imageId).height();
  const auto oldFont = a->renderView->blockLayoutForNode(textId)->reuse.resources.value(LayoutResources::fontKey());
  action(f.window, QStringLiteral("Night"))->trigger();
  store(90);
  resources.publish(LayoutResources::fontKey(), "tab-hidden-font-generation-1");
  action(f.window, QStringLiteral("GitHub"))->trigger();
  store(170);
  resources.publish(LayoutResources::fontKey(), "tab-hidden-font-generation-2");
  QApplication::processEvents();
  require(a->pendingTheme.has_value() && !a->renderView->isVisible(), "resource changes leave hidden theme deferred");
  require(a->renderView->nodeRect(imageId).height() == oldHeight &&
              a->renderView->blockLayoutForNode(textId)->reuse.resources.value(LayoutResources::fontKey()) == oldFont,
          "hidden tabs retain geometry and font dependencies across repeated resource and theme changes");
  const auto revision = a->session.document().revision();
  f.tabs->setCurrentWidget(a);
  require(!a->pendingTheme && a->renderView->theme().backgroundColor() == b->renderView->theme().backgroundColor(),
          "activation installs the newest deferred theme");
  require(a->renderView->nodeRect(imageId).height() >= 170 &&
              a->renderView->blockLayoutForNode(textId)->reuse.resources.value(LayoutResources::fontKey()) ==
                  resources.read(LayoutResources::fontKey()),
          "activation merges latest font and image resources before returning");
  const auto* activated = a->renderView->blockLayoutForNode(textId);
  QApplication::processEvents();
  require(a->renderView->blockLayoutForNode(textId) == activated,
          "queued show refresh does not discard the already current themed layout");
  // Let Qt settle the theme's scrollbar/style geometry before comparing views.
  const auto caret = a->renderView->effectiveCursorRect();
  const auto hit = a->renderView->hitTest(a->renderView->mapDocumentToViewport(caret.center()));
  // Inherit the same window stylesheet/font environment as the tab view.
  // A standalone macOS scroll area has different native scrollbar geometry.
  EditorView fresh(a);
  fresh.resize(a->renderView->size());
  fresh.setZoomPercent(a->renderView->theme().zoomPercent());
  fresh.setFontSizePx(a->renderView->theme().fontSizePx());
  fresh.setContentWidthPx(a->renderView->theme().contentWidthPx());
  fresh.setTheme(a->renderView->theme());
  fresh.setDocument(a->session.document(), a->session.filePath());
  fresh.show();
  fresh.setCursorPosition(cursor);
  QApplication::processEvents();
  if (fresh.viewport()->size() != a->renderView->viewport()->size() || fresh.effectiveCursorRect() != caret) {
    qWarning() << "Activated viewport/caret" << a->renderView->viewport()->size() << caret
               << "fresh viewport/caret" << fresh.viewport()->size() << fresh.effectiveCursorRect();
  }
  require(fresh.viewport()->size() == a->renderView->viewport()->size() && fresh.effectiveCursorRect() == caret,
          "activated layout and font metrics agree with a newly built view");
  const auto expectedHit = fresh.hitTest(fresh.mapDocumentToViewport(fresh.effectiveCursorRect().center()));
  require(hit.blockId == textId && hit.sourceOffset == expectedHit.sourceOffset &&
              hit.sourceOffset == cursor.text.sourceOffset &&
              a->renderView->cursorPosition().text.sourceOffset == cursor.text.sourceOffset &&
              a->session.document().revision() == revision,
          "coalesced activation preserves source cursor and fresh-view hit coordinates");
  fresh.hide();

  // The rendered view can also be hidden inside the active tab's source stack.
  action(f.window, QStringLiteral("Source Code Mode"))->trigger();
  require(!a->renderView->isVisible(), "source mode hides rendered resources");
  store(220);
  QApplication::processEvents();
  require(a->renderView->nodeRect(imageId).height() < 220, "source mode also defers image reflow");
  action(f.window, QStringLiteral("Source Code Mode"))->trigger();
  require(a->renderView->nodeRect(imageId).height() >= 220,
          "return to rendered mode consumes resources before scrolling");
}

QStringList openPaths(Fixture& f) {
  QStringList paths;
  for (int i = 0; i < f.tabs->count(); ++i)
    paths.append(static_cast<DocumentTab*>(f.tabs->widget(i))->session.filePath());
  return paths;
}

void sessionRestoresOrderActiveFolderAndExpansion() {
  SettingsOverride startup("files/startupBehavior", 1);
  SettingsOverride session("session/window", QVariantMap{});
  Fixture original;
  require(QDir(original.directory.path()).mkpath("Folder/Nested/Deep"), "create session folders");
  const QString a = original.file("a.md", "# First\n");
  const QString b = original.file("Folder/b.md", "# Second\n");
  const QString c = original.file("c.md", "# Third\n");
  original.open(a);
  auto* second = original.open(b);
  original.open(c);
  original.tabs->tabBar()->moveTab(2, 0);
  original.tabs->tabBar()->moveTab(1, 2);
  original.tabs->setCurrentWidget(second);
  auto* sidebar = original.window.findChild<SidebarWidget*>();
  sidebar->restoreExpandedFolders({"Folder", "Folder/Nested"});
  auto* tree = sidebar->findChild<QTreeView*>("FileTree");
  auto* model = qobject_cast<FileTreeModel*>(tree->model());
  waitUntil([&] { return tree->isExpanded(model->index(original.directory.filePath("Folder/Nested"))); },
            "session folders expanded before exit");
  tree->setExpanded(model->index(original.directory.filePath("Folder")), false);
  const QStringList expected{c, b, a};
  require(original.window.close(), "clean session closes");
  const auto snapshot = QSettings().value("session/window").toMap();
  require(snapshot.value("files").toStringList() == expected && snapshot.value("active").toInt() == 1,
          "exit preserves tab order and original active tab rather than the last prompted tab");
  require(snapshot.value("folder").toString() == original.directory.path() &&
              snapshot.value("expanded").toStringList() == QStringList({"Folder/Nested"}),
          "exit stores the workspace and relative expanded folder paths");

  Fixture restored;
  restored.window.restoreStartupFile();
  waitUntil([&] {
    for (int i = 0; i < restored.tabs->count(); ++i)
      if (static_cast<DocumentTab*>(restored.tabs->widget(i))->session.isAsyncParseInProgress()) return false;
    return true;
  }, "restored documents finish loading");
  require(openPaths(restored) == expected && restored.current()->session.filePath() == b,
          "startup restores ordered tabs and active file");
  auto* restoredSidebar = restored.window.findChild<SidebarWidget*>();
  auto* restoredTree = restoredSidebar->findChild<QTreeView*>("FileTree");
  auto* restoredModel = qobject_cast<FileTreeModel*>(restoredTree->model());
  waitUntil([&] { return restoredTree->isExpanded(restoredModel->index(original.directory.filePath("Folder/Nested"))); },
            "startup restores asynchronously loaded nested folder expansion");
  require(!restoredTree->isExpanded(restoredModel->index(original.directory.filePath("Folder"))),
          "exit prompts and queued parse notifications preserve a collapsed folder containing the active file");
  require(restoredSidebar->folderRoot() == original.directory.path(), "startup restores the workspace root");
}

void closedFilesAndFolderStayClosed() {
  SettingsOverride startup("files/startupBehavior", 1);
  SettingsOverride session("session/window", QVariantMap{});
  Fixture f;
  const QString a = f.file("a.md", "first\n");
  const QString b = f.file("b.md", "second\n");
  auto* first = f.open(a);
  auto* second = f.open(b);
  f.close(first);
  require(QSettings().value("session/window").toMap().value("files").toStringList() == QStringList{b},
          "closing a file immediately removes it from the persisted session");
  closeFolder(f.window);
  auto* sidebar = f.window.findChild<SidebarWidget*>();
  require(sidebar->folderRoot().isEmpty() && f.current() == second,
          "closing a workspace retains its open documents");
  sidebar->setCurrentDocument("b.md", b, false);
  require(sidebar->folderRoot().isEmpty(), "document refresh does not reopen an explicitly closed workspace");
  Fixture filesOnly;
  filesOnly.window.restoreStartupFile();
  require(openPaths(filesOnly) == QStringList{b} && filesOnly.window.findChild<SidebarWidget*>()->folderRoot().isEmpty(),
          "files can be restored with no workspace");
  action(f.window, "Close Tab")->trigger();
  const auto closed = QSettings().value("session/window").toMap();
  require(closed.value("files").toStringList().isEmpty() && closed.value("folder").toString().isEmpty(),
          "closing the last file and workspace immediately stores an empty session");
  Fixture blank;
  blank.window.restoreStartupFile();
  require(blank.tabs->count() == 1 && blank.current()->session.filePath().isEmpty() &&
              blank.current()->session.markdownText().isEmpty() && !blank.tabs->tabBar()->isVisible() &&
              blank.window.findChild<SidebarWidget*>()->folderRoot().isEmpty(),
          "an empty session starts clean and ignores recent-file history");
}

void startupPreferenceAndMissingFiles() {
  SettingsOverride startup("files/startupBehavior", 0);
  SettingsOverride session("session/window", QVariantMap{});
  Fixture source;
  const QString a = source.file("available.md", "# Available\n");
  const QVariantMap snapshot{{"version", 1}, {"files", QStringList{source.directory.filePath("gone.md"), a}},
                            {"active", 1}, {"folder", source.directory.filePath("removed")}};
  QSettings().setValue("session/window", snapshot);
  Fixture fresh;
  fresh.window.restoreStartupFile();
  require(fresh.current()->session.filePath().isEmpty() && fresh.tabs->count() == 1,
          "always start fresh ignores the saved session");
  QSettings().remove("files/startupBehavior");
  Fixture restored;
  restored.window.restoreStartupFile();
  require(openPaths(restored) == QStringList{a} && restored.current()->session.filePath() == a &&
              restored.window.findChild<SidebarWidget*>()->folderRoot().isEmpty(),
          "default restores valid files while silently skipping removed files and folders");
  QSettings().setValue("session/window", QVariantMap{{"version", 1}, {"files", QStringList{}},
                                                   {"folder", source.directory.path()}});
  Fixture folderOnly;
  folderOnly.window.restoreStartupFile();
  require(folderOnly.current()->session.filePath().isEmpty() && folderOnly.tabs->count() == 1 &&
              folderOnly.window.findChild<SidebarWidget*>()->folderRoot() == source.directory.path(),
          "a workspace can be restored with no open files");
}

void canceledClosePreservesSessionAndActiveTab() {
  SettingsOverride session("session/window", QVariantMap{});
  Fixture f;
  auto* dirty = f.open(f.file("dirty.md", "before\n"));
  auto* active = f.open(f.file("active.md", "active\n"));
  dirty->session.document().setModified(true);
  const auto before = QSettings().value("session/window");
  {
    AnswerPrompts answer(QMessageBox::Cancel);
    f.close(dirty);
    require(answer.count == 1, "closing a dirty background tab prompts");
  }
  require(f.current() == active && QSettings().value("session/window") == before,
          "canceling tab close preserves the active tab and session");
  {
    AnswerPrompts answer(QMessageBox::Cancel);
    require(!f.window.close() && answer.count == 1, "canceling window close keeps it open");
  }
  require(f.current() == active && QSettings().value("session/window") == before,
          "canceling exit does not persist temporary prompt activations");
}

void sessionPreservesEmptyTabAndExplicitReveal() {
  SettingsOverride startup("files/startupBehavior", 1);
  SettingsOverride session("session/window", QVariantMap{});
  Fixture f;
  const QString a = f.file("first.md", "first\n");
  const QString b = f.file("last.md", "last\n");
  f.open(a);
  action(f.window, "New")->trigger();
  auto* blank = f.current();
  f.open(b);
  // Opening a file may reuse an empty tab. Create the placeholder after both
  // real files are open, then move it between them.
  if (blank->session.filePath() == b) action(f.window, "New")->trigger();
  f.tabs->tabBar()->moveTab(2, 1);
  f.tabs->setCurrentIndex(1);
  require(f.current()->session.filePath().isEmpty(), "middle tab is an empty placeholder");
  require(f.window.close(), "session with an empty tab closes");
  Fixture restored;
  restored.window.restoreStartupFile();
  require(openPaths(restored) == QStringList({a, QString(), b}) && restored.tabs->currentIndex() == 1,
          "empty tabs retain their order and active selection among named files");
  restored.tabs->setCurrentIndex(0);
  closeFolder(restored.window);
  action(restored.window, "Show in Sidebar")->trigger();
  require(restored.window.findChild<SidebarWidget*>()->folderRoot() == f.directory.path(),
          "explicit Show in Sidebar can reopen a closed workspace");
}

QString readingFixtureText() {
  QString text;
  for (int i = 0; i < 180; ++i)
    text += QStringLiteral("Paragraph %1 中文 😀 with **bold**, *italic*, $$E=mc^2$$ and words that wrap across the viewport. ").arg(i).repeated(3) + "\n\n";
  return text;
}

void waitForRestoredView(DocumentTab* document) {
  waitUntil([&] { return !document->session.isAsyncParseInProgress() && !document->pendingViewState; },
            "parsed visible tab consumes its pending view state");
  QApplication::processEvents();
}

void requireReadingPosition(DocumentTab* document, const ViewportPosition& expected) {
  if (document->backend->isSourceMode()) {
    const auto actual = document->editor->viewportPosition();
    require(actual.sourceOffset == expected.sourceOffset && qAbs(actual.screenOffset - expected.screenOffset) <= 2,
            "source reading anchor and screen offset survive restart independently of the caret");
  } else {
    const auto cursor = document->editorController.cursorForSourceOffset(expected.sourceOffset);
    const auto hit = document->renderView->hitForCursorPosition(cursor);
    const auto* node = document->session.document().topLevelBlockAtOffset(expected.sourceOffset);
    const qreal y = expected.textAnchor ? hit.cursorRect.top() : document->renderView->nodeRect(node->id()).top();
    const qreal actual = document->renderView->mapDocumentToViewport(QPointF(0, y)).y();
    require(qAbs(actual - expected.screenOffset) <= 2,
            "rendered source anchor stays at its saved screen offset after reparse or rewrap");
  }
}

void sessionRestoresSelectionsModesAndIndependentReadingPositions() {
  SettingsOverride startup("files/startupBehavior", 1);
  SettingsOverride session("session/window", QVariantMap{});
  Fixture original;
  const QString text = readingFixtureText();
  auto* rendered = original.open(original.file("rendered.md", text));
  const auto focus = text.indexOf("Paragraph 20") + 6;
  const auto anchor = focus + 14;
  rendered->editorController.selection().setSelection(
      {rendered->editorController.cursorForSourceOffset(anchor), rendered->editorController.cursorForSourceOffset(focus)});
  const auto savedSelection = rendered->editorController.selection().selection();
  const auto readingOffset = text.indexOf("Paragraph 130");
  rendered->renderView->scrollToNode(rendered->session.document().topLevelBlockAtOffset(readingOffset)->id());
  rendered->renderView->verticalScrollBar()->setValue(rendered->renderView->verticalScrollBar()->value() + 41);
  QApplication::processEvents();
  const auto renderedPosition = rendered->renderView->viewportPosition();
  require(!renderedPosition.atStart && renderedPosition.sourceOffset > anchor, "rendered viewport is far below its selection");

  auto* source = original.open(original.file("source.md", text));
  action(original.window, "Source Code Mode")->trigger();
  source->editor->setCursorPosition(readingOffset);
  source->editor->centerCursor();
  QApplication::processEvents();
  const auto sourcePosition = source->editor->viewportPosition();
  source->editor->setSelection(anchor, focus);
  source->editor->restoreViewportPosition(sourcePosition);
  const auto savedSourcePosition = source->editor->viewportPosition();
  // Leave the rendered page active; the source page must remain deferred after restart.
  original.tabs->setCurrentWidget(rendered);
  require(original.window.close(), "mixed-mode session closes");
  const auto saved = QSettings().value("session/window").toMap();
  const auto views = saved.value("views").toList();
  require(views.size() == 2 && !views[0].toMap().value("sourceMode").toBool() && views[1].toMap().value("sourceMode").toBool(),
          "session persists each tab's own mode");

  Fixture restored;
  restored.window.resize(760, 620);
  restored.window.restoreStartupFile();
  auto* active = restored.current();
  waitForRestoredView(active);
  const auto actualSelection = active->editorController.selection().selection();
  require(actualSelection.anchor.text.sourceOffset == savedSelection.anchor.text.sourceOffset &&
              actualSelection.focus.text.sourceOffset == savedSelection.focus.text.sourceOffset &&
              actualSelection.anchor.text.sourceOffset > actualSelection.focus.text.sourceOffset,
          "rendered reverse selection restores with newly parsed node ids");
  require(!active->backend->isSourceMode() && !action(restored.window, "Source Code Mode")->isChecked(),
          "active tab's restored mode updates the shared mode controls");
  requireReadingPosition(active, renderedPosition);
  auto* hidden = static_cast<DocumentTab*>(restored.tabs->widget(1));
  require(hidden->pendingViewState.has_value() && hidden->backend->isSourceMode(), "background page defers geometry restoration");
  // A session save before first activation must keep the unconsumed state.
  closeFolder(restored.window);
  require(QSettings().value("session/window").toMap().value("views").toList()[1] == views[1],
          "saving an unopened hidden page retains its original pending view state");
  restored.tabs->setCurrentWidget(hidden);
  waitForRestoredView(hidden);
  require(hidden->editor->anchorPosition() == anchor && hidden->editor->cursorPosition() == focus &&
              action(restored.window, "Source Code Mode")->isChecked(),
          "source mode preserves reverse selection and updates shared controls on activation");
  // Width changes can move this source character to another wrapped row; use
  // its geometry at the new width rather than an absolute scrollbar value.
  const auto recaptured = hidden->editor->viewportPosition();
  require(qAbs(hidden->session.markdownText().lineForOffset(recaptured.sourceOffset) -
               hidden->session.markdownText().lineForOffset(savedSourcePosition.sourceOffset)) <= 1,
          "source viewport returns to the saved logical line after width changes");
  const auto before = hidden->editor->viewportPosition();
  restored.tabs->setCurrentWidget(active);
  restored.tabs->setCurrentWidget(hidden);
  QApplication::processEvents();
  require(hidden->editor->viewportPosition().sourceOffset == before.sourceOffset,
          "later tab activations do not replay the old persisted viewport");
  hidden->editor->insertText(QStringLiteral("replacement"));
  QString edited = text;
  edited.replace(focus, anchor - focus, QStringLiteral("replacement"));
  require(hidden->session.markdownText().toString() == edited, "typing replaces the restored source selection at the original coordinates");
}

void sessionViewRestorationWaitsForShowAndBoundsChangedFiles() {
  SettingsOverride startup("files/startupBehavior", 1);
  SettingsOverride session("session/window", QVariantMap{});
  SettingsOverride geometry("window/geometry", QByteArray{});
  Fixture files;
  const auto path = files.file("shortened.md", QStringLiteral("Hello 中文 😀\n"));
  const QVariantMap view{{"version", 1}, {"sourceMode", true}, {"hasCursor", true},
                         {"anchor", qlonglong(9999999)}, {"focus", -8},
                         {"viewport", QVariantMap{{"source", qlonglong(9999999)}, {"screenY", 0}, {"start", false}}}};
  QSettings().setValue("session/window", QVariantMap{{"version", 1}, {"files", QStringList{path}},
      {"active", 0}, {"views", QVariantList{view}}});
  MainWindow restored;
  restored.resize(900, 660);
  restored.restoreStartupFile();
  auto* tabs = restored.findChild<QTabWidget*>("DocumentTabs");
  auto* document = static_cast<DocumentTab*>(tabs->currentWidget());
  require(document->session.isAsyncParseInProgress() && document->pendingViewState.has_value(),
          "restoration retains view state while the worker still owns the unpublished AST");
  QApplication::processEvents();
  require(document->pendingViewState.has_value(), "startup before show keeps saved state pending");
  restored.show();
  waitForRestoredView(document);
  require(document->editor->anchorPosition() == document->session.markdownText().size() && document->editor->cursorPosition() == 0,
          "changed or shortened documents clamp selection endpoints to valid UTF-16 source positions");
  require(!document->session.document().isModified() && !document->editor->canUndo(), "restoration never edits text or adds undo history");
}

void sessionRestoresNestedAndLiteralCoordinates() {
  SettingsOverride startup("files/startupBehavior", 1);
  SettingsOverride session("session/window", QVariantMap{});
  Fixture original;
  const QStringList samples{
      QStringLiteral("# Heading\n\n- outer\n  - Nested 中文 **bold** 😀 text\n\nTail\n"),
      QStringLiteral("| Column | Other |\n| --- | --- |\n| Cell 中文 text | value |\n"),
      QStringLiteral("```cpp\n// Literal 中文 😀 text\nint x = 2;\n```\n"),
      QStringLiteral("$$\n\\text{中文} + x\n$$\n"),
      QStringLiteral("---\ntitle: 中文\n---\n\nBody\n")};
  QList<QPair<qsizetype, qsizetype>> expected;
  for (int i = 0; i < samples.size(); ++i) {
    auto* document = original.open(original.file(QStringLiteral("nested-%1.md").arg(i), samples[i]));
    const auto focus = samples[i].indexOf(QStringLiteral("中文"));
    const auto anchor = focus + 2;
    SelectionRange range{document->editorController.cursorForSourceOffset(anchor), document->editorController.cursorForSourceOffset(focus)};
    if (i >= 2) {
      // Typing through a literal controller also yields block-local source
      // offsets; persistence must canonicalize these rather than save them raw.
      range.anchor.text.sourceOffset = range.anchor.text.textOffset;
      range.focus.text.sourceOffset = range.focus.text.textOffset;
    }
    document->editorController.selection().setSelection(range);
    require(document->editorController.selection().hasCursor(), "nested and literal fixture selection resolves");
    expected.append({anchor, focus});
  }
  require(original.window.close(), "nested and literal session closes");
  Fixture restored;
  restored.window.restoreStartupFile();
  for (int i = 0; i < samples.size(); ++i) {
    restored.tabs->setCurrentIndex(i);
    auto* document = restored.current();
    waitForRestoredView(document);
    const auto selection = document->editorController.selection().selection();
    require(selection.anchor.text.sourceOffset == expected[i].first && selection.focus.text.sourceOffset == expected[i].second,
            "nested list, table cell and code fence selections retain UTF-16 coordinates after restart");
    require(document->editorController.inputController().insertText(QStringLiteral("重启")), "restored literal or nested selection accepts input");
    QString edited = samples[i];
    edited.replace(expected[i].second, 2, QStringLiteral("重启"));
    require(document->session.markdownText().toString() == edited, "typing into a restored nested or literal selection targets its original source");
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  installMuffinTestCrashHandler();
  QStandardPaths::setTestModeEnabled(true);
  QApplication::setOrganizationName(QStringLiteral("MuffinTests"));
  QApplication::setApplicationName(QStringLiteral("DocumentTabsTest"));
  QTemporaryDir settings;
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
  QSettings().setValue(QStringLiteral("update/autoCheck"), false);
  QSettings().setValue(QStringLiteral("appearance/reducedMotion"), true);
  QSettings().setValue(QStringLiteral("files/autoSaveOnSwitch"), false);
  QSettings().setValue(QStringLiteral("files/autoSave"), false);
  LanguageManager::instance().setLanguage(QStringLiteral("en"));
  runTest("sessionRestoresSelectionsModesAndIndependentReadingPositions", sessionRestoresSelectionsModesAndIndependentReadingPositions);
  runTest("sessionViewRestorationWaitsForShowAndBoundsChangedFiles", sessionViewRestorationWaitsForShowAndBoundsChangedFiles);
  runTest("sessionRestoresNestedAndLiteralCoordinates", sessionRestoresNestedAndLiteralCoordinates);
  if (QCoreApplication::arguments().contains(QStringLiteral("--session-view-state"))) return 0;
  runTest("sessionRestoresOrderActiveFolderAndExpansion", sessionRestoresOrderActiveFolderAndExpansion);
  runTest("closedFilesAndFolderStayClosed", closedFilesAndFolderStayClosed);
  runTest("startupPreferenceAndMissingFiles", startupPreferenceAndMissingFiles);
  runTest("canceledClosePreservesSessionAndActiveTab", canceledClosePreservesSessionAndActiveTab);
  runTest("sessionPreservesEmptyTabAndExplicitReveal", sessionPreservesEmptyTabAndExplicitReveal);
  runTest("tabBarOnlyForMultipleDocuments", tabBarOnlyForMultipleDocuments);
  runTest("formatActionsFollowSelectionAndDocumentChanges", formatActionsFollowSelectionAndDocumentChanges);
  runTest("sharedCursorQueriesMatchIndependentQueries", sharedCursorQueriesMatchIndependentQueries);
  runTest("retainsDocumentsAndRenderState", retainsDocumentsAndRenderState);
  runTest("sourceStateAndKeyboardNavigation", sourceStateAndKeyboardNavigation);
  runTest("closesWithSaveDiscardAndCancel", closesWithSaveDiscardAndCancel);
  runTest("backgroundLoadingAndPersistence", backgroundLoadingAndPersistence);
  runTest("openFailuresAndConflictingSaveAs", openFailuresAndConflictingSaveAs);
  runTest("renameFolderAndTranslateTabs", renameFolderAndTranslateTabs);
  runTest("hiddenTabsDeferThemeChanges", hiddenTabsDeferThemeChanges);
  runTest("hiddenTabsMergeThemeAndResourceChanges", hiddenTabsMergeThemeAndResourceChanges);
  return 0;
}
