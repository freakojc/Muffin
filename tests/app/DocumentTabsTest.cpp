#include "app/MainWindow.h"
#include "app/LanguageManager.h"
#include "editor/EditorView.h"
#include "editor/SourceEditorWidget.h"
#include "editor/VirtualSourceEdit.h"

#include "../TestUtils.h"

#include <QAction>
#include <QApplication>
#include <QElapsedTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileDialog>
#include <QMessageBox>
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
#ifdef Q_OS_MACOS
    window.activateWindow();
    require(QTest::qWaitForWindowActive(&window), "native test window becomes active for keyboard shortcuts");
#endif
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
  const auto drafts = recovery.pendingDrafts();
  require(drafts.size() == 2 && a->draftKey != b->draftKey, "each tab has its own recovery snapshot");
  {
    SettingsOverride autoSave("files/autoSave", true);
    QMetaObject::invokeMethod(a->autoSaveTimer, "timeout", Qt::DirectConnection);
    require(contents(first) == QStringLiteral("Dirty A\n"), "background autosave writes A");
    require(b->session.document().isModified() && contents(second).contains(QStringLiteral("Small")), "autosave leaves B unsaved");
    require(recovery.pendingDrafts().size() == 1, "saving A leaves B recovery snapshot");
  }
  a->session.setMarkdownText(QStringLiteral("Save All A\n"), true);
  action(f.window, QStringLiteral("Save All Open Files..."))->trigger();
  require(contents(first) == QStringLiteral("Save All A\n") && contents(second) == QStringLiteral("Dirty B\n"), "Save All includes inactive tabs");
  require(f.current() == b && recovery.pendingDrafts().isEmpty(), "Save All retains active tab and clears saved drafts");

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
    require(answer.count == 1 && a->session.document().isModified(), "Save As refuses another open document");
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

}  // namespace

int main(int argc, char** argv) {
  if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_MACOS
    qputenv("QT_QPA_PLATFORM", "cocoa");
#else
    qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
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
  runTest("tabBarOnlyForMultipleDocuments", tabBarOnlyForMultipleDocuments);
  runTest("retainsDocumentsAndRenderState", retainsDocumentsAndRenderState);
  runTest("sourceStateAndKeyboardNavigation", sourceStateAndKeyboardNavigation);
  runTest("closesWithSaveDiscardAndCancel", closesWithSaveDiscardAndCancel);
  runTest("backgroundLoadingAndPersistence", backgroundLoadingAndPersistence);
  runTest("openFailuresAndConflictingSaveAs", openFailuresAndConflictingSaveAs);
  runTest("renameFolderAndTranslateTabs", renameFolderAndTranslateTabs);
  return 0;
}
