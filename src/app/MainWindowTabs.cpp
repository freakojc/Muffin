#include "app/MainWindow.h"

#include "app/MarkdownSettings.h"
#include "app/SourceEditorBackend.h"
#include "app/RenderEditorBackend.h"
#include "app/StatusBarWidget.h"
#include "document/BlockPredicates.h"
#include "editor/EditorView.h"
#include "editor/BlockEditContext.h"
#include "editor/SourceEditorWidget.h"

#include <QAction>
#include <QFileInfo>
#include <QMouseEvent>
#include <QScopedValueRollback>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QToolButton>
#include <QTimer>
#include <QVariantMap>

#include <cmath>

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

qsizetype sourceOffset(muffin::DocumentTab* document, const muffin::CursorPosition& cursor) {
  muffin::BlockEditContextResolver resolver(&document->session, &document->editorController.selection());
  const auto* node = document->session.document().node(cursor.blockId);
  // Literal controllers also produce block-local source offsets. Their text
  // offset is authoritative; never persist it as a document-wide position.
  if (node && muffin::isLiteralBlockType(node->type()) && !cursor.afterBlock && !cursor.text.inMeta)
    return resolver.literalContentStartOffset(*node) + cursor.text.textOffset;
  if (cursor.text.sourceOffset >= 0) return cursor.text.sourceOffset;
  muffin::BlockEditContext context;
  const auto id = cursor.text.nodeId.isValid() ? cursor.text.nodeId : cursor.blockId;
  if (resolver.forBlock(id, context)) {
    qsizetype local = 0;
    if (context.inlineProjection.sourceOffsetForVisibleOffset(cursor.text.textOffset, local))
      return context.contentRange.byteStart + local;
  }
  if (const auto* sourceNode = document->session.document().node(id))
    return cursor.afterBlock ? sourceNode->sourceRange().byteEnd : sourceNode->sourceRange().byteStart;
  return 0;
}

QVariantMap literalPosition(muffin::DocumentTab* document, const muffin::CursorPosition& cursor) {
  const auto* node = document->session.document().node(cursor.blockId);
  if (!node || !muffin::isLiteralBlockType(node->type()) || cursor.afterBlock || cursor.text.inMeta) return {};
  return {{QStringLiteral("block"), qlonglong(node->sourceRange().byteStart)},
          {QStringLiteral("offset"), qlonglong(cursor.text.textOffset)}};
}

QVariantMap viewportState(const muffin::ViewportPosition& position) {
  return {{QStringLiteral("source"), qlonglong(position.sourceOffset)},
          {QStringLiteral("screenY"), position.screenOffset}, {QStringLiteral("horizontal"), position.horizontal},
          {QStringLiteral("text"), position.textAnchor}, {QStringLiteral("start"), position.atStart}};
}

muffin::ViewportPosition viewportPosition(const QVariantMap& state) {
  muffin::ViewportPosition position;
  position.sourceOffset = state.value(QStringLiteral("source"), -1).toLongLong();
  const auto y = state.value(QStringLiteral("screenY")).toDouble();
  position.screenOffset = std::isfinite(y) ? qBound(-1000000.0, y, 1000000.0) : 0;
  position.horizontal = qMax(0, state.value(QStringLiteral("horizontal")).toInt());
  position.textAnchor = state.value(QStringLiteral("text")).toBool();
  position.atStart = state.value(QStringLiteral("start")).toBool();
  return position;
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
  saveSessionState();
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
  if (tab_->pendingTheme) {
    auto theme = std::move(*tab_->pendingTheme);
    tab_->pendingTheme.reset();
    tab_->renderView->setTheme(theme);
    tab_->editor->setTheme(theme);
  }
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
  tab_->renderView->refreshPendingResources();
  updateTitle();
  updateStatus();
  updateAllActions();
  refreshSidebarDocumentInfo();
  refreshSidebarOutline();
  updateWordCountNow();
  updateRenderCursorStatus(tab_->editorController.selection().currentHit());
  updateBlockSourceLabel(tab_->editorController.selection().currentHit());
  tab_->viewStack->currentWidget()->setFocus(Qt::OtherFocusReason);
  scheduleSessionSave();
  QTimer::singleShot(0, next, [this, next] { restoreTabViewState(next); });
}

void muffin::MainWindow::closeTab(int index) {
  const QScopedValueRollback<bool> switchGuard(suppressSaveOnTabSwitch_, true);
  const bool updatesWerePaused = sessionUpdatesPaused_;
  const QScopedValueRollback<bool> sessionGuard(sessionUpdatesPaused_, true);
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
  sessionUpdatesPaused_ = updatesWerePaused;
  refreshSidebarDocumentInfo();
  saveSessionState();
}

void muffin::MainWindow::scheduleSessionSave() {
  if (!sessionUpdatesPaused_ && sessionSaveTimer_) sessionSaveTimer_->start();
}

void muffin::MainWindow::saveSessionState() {
  if (sessionUpdatesPaused_ || !tabs_ || !sidebar_) return;
  if (sessionSaveTimer_) sessionSaveTimer_->stop();
  QStringList files;
  QVariantList views;
  int active = -1;
  for (auto* document : documentTabs()) {
    const QString path = document->session.filePath();
    // Unsaved contents belong to draft recovery, never to settings. Keep empty
    // tab placeholders only when they are part of a multi-document session.
    if (path.isEmpty() && (tabs_->count() == 1 || document->session.document().isModified())) continue;
    if (document == tab_) active = files.size();
    files.append(path);
    views.append(tabViewState(document));
  }
  const QVariantMap snapshot{{QStringLiteral("version"), 1},
      {QStringLiteral("files"), files}, {QStringLiteral("active"), active},
      {QStringLiteral("views"), views},
      {QStringLiteral("folder"), sidebar_->folderRoot()},
      {QStringLiteral("expanded"), sidebar_->expandedFolders()}};
  QSettings settings;
  settings.setValue(QStringLiteral("session/window"), snapshot);
  settings.sync();
}

QVariantMap muffin::MainWindow::tabViewState(DocumentTab* document) const {
  if (document->pendingViewState) return *document->pendingViewState;
  const bool source = document->backend->isSourceMode();
  QVariantMap state{{QStringLiteral("version"), 1}, {QStringLiteral("sourceMode"), source}};
  if (source) {
    state.insert(QStringLiteral("anchor"), qlonglong(document->editor->anchorPosition()));
    state.insert(QStringLiteral("focus"), qlonglong(document->editor->cursorPosition()));
    state.insert(QStringLiteral("hasCursor"), true);
    state.insert(QStringLiteral("viewport"), viewportState(document->editor->viewportPosition()));
  } else {
    const auto& selection = document->editorController.selection();
    state.insert(QStringLiteral("hasCursor"), selection.hasCursor());
    if (selection.hasCursor()) {
      const auto range = selection.selection();
      state.insert(QStringLiteral("anchor"), qlonglong(sourceOffset(document, range.anchor)));
      state.insert(QStringLiteral("focus"), qlonglong(sourceOffset(document, range.focus)));
      state.insert(QStringLiteral("anchorAfter"), range.anchor.afterBlock);
      state.insert(QStringLiteral("focusAfter"), range.focus.afterBlock);
      state.insert(QStringLiteral("anchorMeta"), range.anchor.text.inMeta);
      state.insert(QStringLiteral("focusMeta"), range.focus.text.inMeta);
      state.insert(QStringLiteral("anchorLiteral"), literalPosition(document, range.anchor));
      state.insert(QStringLiteral("focusLiteral"), literalPosition(document, range.focus));
      state.insert(QStringLiteral("htmlEditing"), document->editorController.htmlLiteral().isEditing());
    }
    state.insert(QStringLiteral("viewport"), viewportState(document->renderView->viewportPosition()));
  }
  return state;
}

void muffin::MainWindow::restoreTabViewState(DocumentTab* document) {
  if (!document->pendingViewState || document != tab_ || sessionUpdatesPaused_ ||
      document->session.isAsyncParseInProgress() || !document->isVisible()) return;
  const QScopedValueRollback<bool> guard(restoringViewState_, true);
  const auto state = *document->pendingViewState;
  const auto& text = document->session.markdownText();
  const auto bounded = [&](qlonglong offset) {
    auto result = qBound<qsizetype>(0, offset, text.size());
    if (result > 0 && result < text.size() && text.at(result).isLowSurrogate() && text.at(result - 1).isHighSurrogate()) --result;
    return result;
  };
  const auto anchor = bounded(state.value(QStringLiteral("anchor")).toLongLong());
  const auto focus = bounded(state.value(QStringLiteral("focus")).toLongLong());
  auto viewport = viewportPosition(state.value(QStringLiteral("viewport")).toMap());
  if (viewport.sourceOffset >= 0) viewport.sourceOffset = bounded(viewport.sourceOffset);
  if (document->backend->isSourceMode()) {
    document->editor->setSelection(anchor, focus);
    document->editor->restoreViewportPosition(viewport);
  } else {
    if (document->renderViewDirty) {
      document->renderView->setDocument(document->session.document(), document->session.filePath());
      document->renderViewDirty = false;
    }
    if (state.value(QStringLiteral("hasCursor")).toBool()) {
      SelectionRange range{document->editorController.cursorForSourceOffset(anchor),
                           document->editorController.cursorForSourceOffset(focus)};
      const auto restoreLiteral = [&](CursorPosition& cursor, const QVariantMap& saved) {
        if (saved.isEmpty()) return;
        auto target = document->editorController.cursorForSourceOffset(bounded(saved.value(QStringLiteral("block")).toLongLong()));
        const auto* node = document->session.document().node(target.blockId);
        if (!node || !isLiteralBlockType(node->type())) return;
        const auto literal = node->literal();
        target.text.textOffset = qBound<qsizetype>(0, saved.value(QStringLiteral("offset")).toLongLong(), literal.size());
        if (target.text.textOffset > 0 && target.text.textOffset < literal.size() &&
            literal.at(target.text.textOffset).isLowSurrogate() && literal.at(target.text.textOffset - 1).isHighSurrogate())
          --target.text.textOffset;
        cursor = target;
        cursor.text.sourceOffset = sourceOffset(document, cursor);
      };
      restoreLiteral(range.anchor, state.value(QStringLiteral("anchorLiteral")).toMap());
      restoreLiteral(range.focus, state.value(QStringLiteral("focusLiteral")).toMap());
      range.anchor.afterBlock = state.value(QStringLiteral("anchorAfter")).toBool();
      range.focus.afterBlock = state.value(QStringLiteral("focusAfter")).toBool();
      range.anchor.text.inMeta = state.value(QStringLiteral("anchorMeta")).toBool();
      range.focus.text.inMeta = state.value(QStringLiteral("focusMeta")).toBool();
      const auto hit = document->renderView->hitForCursorPosition(range.focus);
      if (hit.zone == HitTestResult::Zone::Code || hit.zone == HitTestResult::Zone::Math ||
          hit.zone == HitTestResult::Zone::FrontMatter ||
          (hit.zone == HitTestResult::Zone::Html && state.value(QStringLiteral("htmlEditing")).toBool())) {
        document->editorController.activateHit(hit);
        if (hit.zone == HitTestResult::Zone::Html) document->editorController.enterHtmlEditMode();
      }
      document->editorController.selection().setSelection(range);
    } else {
      document->editorController.selection().clear();
    }
    document->renderView->restoreViewportPosition(viewport, document->editorController.cursorForSourceOffset(viewport.sourceOffset));
  }
  document->pendingViewState.reset();
}

void muffin::MainWindow::closeFolder() {
  sidebarFolderRoot_.clear();
  sidebar_->setFolderRoot({});
  saveSessionState();
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
