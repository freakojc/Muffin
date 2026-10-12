#include "app/MainWindow.h"

#include "app/MarkdownSettings.h"
#include "app/SourceEditorBackend.h"
#include "app/StatusBarWidget.h"
#include "editor/EditorView.h"
#include "editor/SourceEditorWidget.h"

#include <QAction>
#include <QFileInfo>
#include <QSettings>
#include <QScopedValueRollback>
#include <QMouseEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>

namespace {

QString pathKey(const QString& path) {
  if (path.isEmpty()) {
    return {};
  }
  const QFileInfo info(path);
  QString key = info.canonicalFilePath();
  if (key.isEmpty()) {
    key = info.absoluteFilePath();
  }
#ifdef Q_OS_WIN
  key = key.toCaseFolded();
#endif
  return key;
}

}  // namespace

QList<muffin::DocumentTab*> muffin::MainWindow::documentTabs() const {
  QList<DocumentTab*> documents;
  for (int i = 0; i < tabs_->count(); ++i) {
    documents.append(static_cast<DocumentTab*>(tabs_->widget(i)));
  }
  return documents;
}

bool muffin::MainWindow::isPathOpen(const QString& path, const DocumentSession* except) const {
  const QString key = pathKey(path);
  for (DocumentTab* document : documentTabs()) {
    if (&document->session != except && !key.isEmpty() && pathKey(document->session.filePath()) == key) {
      return true;
    }
  }
  return false;
}

void muffin::MainWindow::configureTab(DocumentTab* document) {
  const RenderTheme theme = themeManager_.currentTheme(zoomPercent_, fontSizePx_);
  document->renderView->setTheme(theme);
  document->editor->setTheme(theme);
  document->renderView->setZoomPercent(zoomPercent_);
  document->editor->setZoomPercent(zoomPercent_);
  document->renderView->setFontSizePx(fontSizePx_);
  document->editor->setFontSizePx(fontSizePx_);
  document->renderView->setContentWidthPx(contentWidthPx_);
  document->editor->setContentWidthPx(contentWidthPx_);
  document->renderView->setFocusMode(focusMode_);
  document->renderView->setTypewriterMode(typewriterMode_);
  document->renderView->setTypewriterCursorMiddle(
      QSettings().value(QStringLiteral("editor/typewriterCursorMiddle"), true).toBool());
  if (const QAction* wrap = commands_.action(QStringLiteral("view.word_wrap"))) {
    document->editor->setWordWrapEnabled(wrap->isChecked());
  }
}

muffin::DocumentTab* muffin::MainWindow::createDocumentTab() {
  auto* document = new DocumentTab(tabs_);
  document->session.setParseOptions(markdownParseOptions());
  document->editorController.inputController().setEmojiProvider(&emojiProvider_);
  configureTab(document);
  if (tab_->backend->isSourceMode()) {
    document->backend = std::make_unique<SourceEditorBackend>(document->editor);
    document->viewStack->setCurrentWidget(document->editor);
  }
  connectEditorSignals(document);
  connectRenderSignals(document);
  connectSessionSignals(document);
  connectFindBarSignals(document);
  document->renderView->setDocument(document->session.document(), {});
  // The first tab remains active until the new page has been initialized.
  const QSignalBlocker blocker(tabs_);
  tabs_->addTab(document, document->session.displayName());
  setupTabButton(document);
  updateTabLabel(document);
  updateTabBarVisibility();
  return document;
}

bool muffin::MainWindow::eventFilter(QObject* watched, QEvent* event) {
  if (watched == tabs_->tabBar() && event->type() == QEvent::MouseButtonRelease) {
    auto* mouse = static_cast<QMouseEvent*>(event);
    if (mouse->button() == Qt::MiddleButton) {
      closeTab(tabs_->tabBar()->tabAt(mouse->position().toPoint()));
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void muffin::MainWindow::setupTabButton(DocumentTab* document) {
  const int index = tabs_->indexOf(document);
  if (QWidget* old = tabs_->tabBar()->tabButton(index, QTabBar::RightSide)) {
    old->deleteLater();
  }
  auto* button = new QToolButton(tabs_->tabBar());
  button->setObjectName(QStringLiteral("CloseTabButton"));
  button->setText(QString(QChar(0x00d7)));
  button->setFixedSize(20, 20);
  button->setAutoRaise(true);
  button->setToolTip(tr("Close Tab"));
  button->setAccessibleName(tr("Close Tab"));
  tabs_->tabBar()->setTabButton(index, QTabBar::RightSide, button);
  connect(button, &QToolButton::clicked, document, [this, document] {
    closeTab(tabs_->indexOf(document));
  });
}

void muffin::MainWindow::updateTabBarVisibility() {
  const bool multipleDocuments = tabs_->count() > 1;
  tabs_->tabBar()->setVisible(multipleDocuments);
  // A visible corner widget reserves a row even when QTabBar auto-hides.
  if (QWidget* button = tabs_->cornerWidget()) {
    button->setVisible(multipleDocuments);
  }
}

void muffin::MainWindow::retranslateTabs() {
  tabs_->tabBar()->setAccessibleName(tr("Open Documents"));
  auto* button = static_cast<QToolButton*>(tabs_->cornerWidget());
  if (button) {
    button->setToolTip(tr("New Tab") + QStringLiteral(" (%1)")
        .arg(QKeySequence(QKeySequence::New).toString(QKeySequence::NativeText)));
    button->setAccessibleName(tr("New Tab"));
  }
  for (DocumentTab* document : documentTabs()) {
    updateTabLabel(document);
    if (QWidget* close = tabs_->tabBar()->tabButton(tabs_->indexOf(document), QTabBar::RightSide)) {
      close->setToolTip(tr("Close Tab"));
      close->setAccessibleName(tr("Close Tab"));
    }
  }
}

bool muffin::MainWindow::openFile(QString path) {
  if (path.isEmpty()) {
    path = tab_->fileController.chooseOpenPath(this);
    if (path.isEmpty()) {
      return false;
    }
  }
  const QString key = pathKey(path);
  for (DocumentTab* document : documentTabs()) {
    if (pathKey(document->session.filePath()) == key) {
      tabs_->setCurrentWidget(document);
      document->viewStack->currentWidget()->setFocus();
      addRecentFile(document->session.filePath());
      return true;
    }
  }
  // Reuse the pristine startup page; an edited untitled document is always retained.
  const bool reuse = tab_->session.filePath().isEmpty() && !tab_->session.document().isModified()
      && tab_->session.markdownText().isEmpty() && !tab_->session.isAsyncParseInProgress()
      && !tab_->editorController.canUndo() && !tab_->editorController.canRedo()
      && !tab_->editor->canUndo() && !tab_->editor->canRedo();
  DocumentTab* document = reuse ? tab_ : createDocumentTab();
  if (!document->fileController.open(document->session, this, QFileInfo(path).absoluteFilePath())) {
    if (!reuse) {
      const QSignalBlocker blocker(tabs_);
      tabs_->removeTab(tabs_->indexOf(document));
      updateTabBarVisibility();
      delete document;
    }
    return false;
  }
  document->editorController.clearHistoryAndSelection();
  tabs_->setCurrentWidget(document);
  updateTabLabel(document);
  addRecentFile(document->session.filePath());
  return true;
}

bool muffin::MainWindow::startNewDocument() {
  DocumentTab* document = createDocumentTab();
  tabs_->setCurrentWidget(document);
  return true;
}

void muffin::MainWindow::activateTab(int index) {
  if (index < 0) {
    return;
  }
  auto* next = static_cast<DocumentTab*>(tabs_->widget(index));
  if (tab_ != next && !suppressSaveOnTabSwitch_ && !tab_->session.filePath().isEmpty()
      && tab_->session.document().isModified()
      && QSettings().value(QStringLiteral("files/autoSaveOnSwitch"), false).toBool()) {
    tab_->fileController.save(tab_->session, this);
  }
  tab_ = next;
  wordCountTimer_->stop();
  outlineTimer_->stop();
  tab_->outlineDirty = true;  // the shared sidebar last displayed another tab
  tab_->wordCountDirty = true;
  if (QAction* action = commands_.action(QStringLiteral("view.source_mode"))) {
    const QSignalBlocker blocker(action);
    action->setChecked(tab_->backend->isSourceMode());
  }
  sourceModeButton_->setChecked(tab_->backend->isSourceMode());
  if (tab_->renderViewDirty && !tab_->backend->isSourceMode() && !tab_->session.isAsyncParseInProgress()) {
    tab_->renderView->setDocument(tab_->session.document(), tab_->session.filePath());
    tab_->renderViewDirty = false;
  }
  updateTitle();
  updateStatus();
  updateAllActions();
  refreshSidebarDocumentInfo();
  refreshSidebarOutline();
  updateWordCountNow();
  updateRenderCursorStatus(tab_->editorController.selection().currentHit());
  updateBlockSourceLabel(tab_->editorController.selection().currentHit());
  tab_->viewStack->currentWidget()->setFocus(Qt::OtherFocusReason);
}

void muffin::MainWindow::closeTab(int index) {
  const QScopedValueRollback<bool> switchGuard(suppressSaveOnTabSwitch_, true);
  if (index < 0 || index >= tabs_->count()) {
    return;
  }
  auto* document = static_cast<DocumentTab*>(tabs_->widget(index));
  DocumentTab* original = tab_;
  tabs_->setCurrentWidget(document);
  performAutoSave(document);
  snapshotDraft(document);
  if (!maybeSaveChanges()) {
    tabs_->setCurrentWidget(original);
    return;
  }
  const bool closingOriginal = document == original;
  if (tabs_->count() == 1) {
    createDocumentTab();
  }
  tabs_->removeTab(tabs_->indexOf(document));
  updateTabBarVisibility();
  if (!closingOriginal) {
    tabs_->setCurrentWidget(original);
  }
  delete document;
}

void muffin::MainWindow::cycleTab(bool forward) {
  const int count = tabs_->count();
  tabs_->setCurrentIndex((tabs_->currentIndex() + (forward ? 1 : count - 1)) % count);
}

void muffin::MainWindow::updateTabLabel(DocumentTab* document) {
  const int index = tabs_->indexOf(document);
  if (index < 0) {
    return;
  }
  const QString name = document->session.displayName();
  tabs_->setTabText(index, name + (document->session.document().isModified() ? QStringLiteral(" *") : QString()));
  tabs_->setTabToolTip(index, document->session.filePath().isEmpty() ? name : document->session.filePath());
}
