#include "app/DocumentTab.h"

#include "app/RenderEditorBackend.h"
#include "editor/EditorView.h"
#include "editor/FindBarWidget.h"
#include "editor/SourceEditorWidget.h"

#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

muffin::DocumentTab::DocumentTab(QWidget* parent)
    : QWidget(parent), renderCommands(editorController, [this] { return backend && backend->isSourceMode(); }) {
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(0);
  viewStack = new QStackedWidget(this);
  renderView = new EditorView(viewStack);
  editor = new SourceEditorWidget(viewStack);
  editor->bindSession(&session);
  viewStack->addWidget(renderView);
  viewStack->addWidget(editor);
  backend = std::make_unique<RenderEditorBackend>(editorController, session, renderView);
  findBar = new FindBarWidget(this);
  findBar->hide();
  layout->addWidget(findBar);
  layout->addWidget(viewStack, 1);
  autoSaveTimer = new QTimer(this);
  autoSaveTimer->setSingleShot(true);
  autoSaveTimer->setInterval(1500);
  draftTimer = new QTimer(this);
  draftTimer->setSingleShot(true);
  draftTimer->setInterval(3000);
  editorController.attach(&session, renderView);
}

muffin::DocumentTab::~DocumentTab() {
  // Views refer to the session/controller; destroy them while those members are alive.
  disconnect(&editorController, nullptr, this, nullptr);
  disconnect(&session, nullptr, this, nullptr);
  editorController.detach();
  backend.reset();
  delete findBar;
  delete viewStack;
}
