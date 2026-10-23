#include "EditorViewTestUtils.h"

#include <QEventLoop>
#include <QScrollBar>
#include <QTimer>

using namespace muffin;

// Regression for the rendering bug where an unchecked task item (isTaskItem()==true,
// taskChecked()==false) rendered as a plain bullet instead of an empty checkbox,
// because the layout's "is a task item" flag was driven off taskChecked().
void testUncheckedTaskItemRendersAsCheckbox() {
  DocumentSession session;
  EditorController controller;
  EditorView view;
  controller.attach(&session, &view);

  session.setMarkdownText(QStringLiteral("- [ ] a\n- [x] b"), false);
  view.resize(800, 240);
  view.setDocument(session.document());

  // Look up the list-item layout directly: blockAtViewportPos would return the
  // innermost block (the paragraph child), which is not the task-item layout.
  const BlockLayout* unchecked = view.blockLayoutForNode(listItemAt(session, 0, 0)->id());
  require(unchecked != nullptr, "unchecked item layout should exist");
  require(unchecked->isTaskListItem(), "an unchecked task item must render as a checkbox, not a bullet");
  require(!unchecked->taskChecked(), "unchecked item should be unchecked");

  const BlockLayout* checked = view.blockLayoutForNode(listItemAt(session, 0, 1)->id());
  require(checked != nullptr, "checked item layout should exist");
  require(checked->isTaskListItem(), "a checked task item must still render as a checkbox");
  require(checked->taskChecked(), "checked item should be checked");
}

// Clicking inside the checkbox affordance flags the hit so the view toggles the
// item; clicking the content text does not.
void testCheckboxAlignsWithFirstLine() {
  DocumentSession session;
  EditorController controller;
  EditorView view;
  controller.attach(&session, &view);

  session.setMarkdownText(QStringLiteral("- [x] Parse CommonMark blocks"), false);
  view.resize(800, 240);
  view.setDocument(session.document());

  const NodeId itemId = listItemAt(session, 0, 0)->id();
  const BlockLayout* item = view.blockLayoutForNode(itemId);
  require(item != nullptr, "task item layout should exist");
  require(item->isTaskListItem(), "item should be a task item");
  const InlineLayout* inlineLayout = item->inlineLayout();
  require(inlineLayout != nullptr, "task item should have inline text layout");

  const RenderTheme theme = view.theme();
  const QFontMetricsF metrics(theme.paragraphFont());
  const qreal expectedTextCenter =
      item->rect().top() + inlineLayout->firstLineBaselineY() + (metrics.descent() - metrics.ascent()) * 0.5;
  const QRectF box = item->taskCheckboxRect(theme);
  require(qAbs(box.center().y() - expectedTextCenter) < 0.75,
          QStringLiteral("checkbox should be centered on the first rendered text line (box=%1 expected=%2)")
              .arg(box.center().y())
              .arg(expectedTextCenter));
}

void testCheckboxClickTarget() {
  DocumentSession session;
  EditorController controller;
  EditorView view;
  controller.attach(&session, &view);

  session.setMarkdownText(QStringLiteral("- [ ] a"), false);
  view.resize(800, 240);
  view.setDocument(session.document());

  const NodeId itemId = listItemAt(session, 0, 0)->id();
  const BlockLayout* item = view.blockLayoutForNode(itemId);
  require(item != nullptr, "task item layout should exist");
  require(item->isTaskListItem(), "item should be a task item");

  const RenderTheme theme = view.theme();
  const QRectF box = item->taskCheckboxRect(theme);
  require(!box.isEmpty(), "checkbox rect should be non-empty");

  const HitTestResult checkboxHit = view.hitTest(box.center());
  require(checkboxHit.blockId == itemId, "checkbox hit should target the task item");
  require(checkboxHit.taskCheckboxHit, "a click inside the checkbox should be flagged as a toggle hit");

  // A point just past the marker gutter, inside the content text, must not toggle.
  const QRectF itemRect = view.nodeRect(itemId);
  const QPointF contentPoint(itemRect.left() + item->listContentIndent() + 4.0, box.center().y());
  const HitTestResult contentHit = view.hitTest(contentPoint);
  require(!contentHit.taskCheckboxHit, "a click in the content should not toggle");
}

namespace {

void verifyCheckboxClickPreservesViewport(bool hasCaret, bool typewriterMode = false, bool replaceDocuments = false) {
  DocumentSession session;
  EditorController controller;
  EditorView view;
  controller.attach(&session, &view);
  view.resize(800, 300);

  const auto openDocument = [&](const QString& text) {
    QEventLoop loop;
    QObject::connect(&session, &DocumentSession::parsed, &loop, &QEventLoop::quit);
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    session.openDocumentAsync(text);
    loop.exec();
    require(!session.isAsyncParseInProgress(), "document open should finish");
  };

  if (replaceDocuments) {
    // Reuse the view across asynchronous opens, as when opening large files and then a small one.
    QObject::connect(&session, &DocumentSession::parsed, &view, [&] {
      if (!session.lastParseWasLocalEdit()) {
        view.setDocument(session.document());
      }
    });
    for (int count : {1000, 300}) {
      openDocument(QStringLiteral("Previous document paragraph.\n\n").repeated(count));
      controller.setCursorForSourceOffset(2);
      view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
    }
  }

  const QString filler = QStringLiteral("Filler paragraph.\n\n").repeated(40);
  const QString tasks = QStringLiteral("- [x] Parse CommonMark blocks\n"
                                       "- [x] Parse GFM tables and task markers\n"
                                       "- [x] Parse inline and block math\n"
                                       "- [x] Render Markdown from the document AST\n"
                                       "- [x] Edit tables, code fences, math blocks, and HTML blocks\n"
                                       "- [ ] Complete export workflows\n\n");
  const QString original = filler + tasks + filler;
  if (replaceDocuments) {
    openDocument(original);
  } else {
    session.setMarkdownText(original, false);
  }
  view.setDocument(session.document());
  require(!controller.selection().hasCursor(), "opening a document should clear the previous caret");
  if (hasCaret) {
    controller.setCursorForSourceOffset(2);
  }
  view.setTypewriterMode(typewriterMode);
  // Match MainWindow's typewriter-mode cursor-follow connection.
  QObject::connect(&controller, &EditorController::cursorChanged, &view, [&] {
    if (typewriterMode) {
      view.scrollToCursorCenteredAnimated();
    }
  });
  const qsizetype caretBefore = controller.selection().cursorPosition().text.sourceOffset;

  const NodeId itemId = listItemAt(session, 40, 5)->id();
  const BlockLayout* item = view.blockLayoutForNode(itemId);
  require(item != nullptr, "task item layout should exist");
  const QPointF checkboxCenter = item->taskCheckboxRect(view.theme()).center();
  QScrollBar* bar = view.verticalScrollBar();
  bar->setValue(qRound(checkboxCenter.y() - view.viewport()->height() / 2.0));
  const int scrollBefore = bar->value();
  require(scrollBefore > view.viewport()->height(), "caret should be scrolled out of view");
  const QPointF click = view.mapDocumentToViewport(checkboxCenter);
  require(view.hitTest(click).taskCheckboxHit, "click should hit the scrolled checkbox");

  QMouseEvent press(QEvent::MouseButtonPress, click, click, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &press);
  require(bar->value() == scrollBefore, "checkbox click must not scroll to the old caret");
  QMouseEvent release(QEvent::MouseButtonRelease, click, click, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
  QApplication::sendEvent(view.viewport(), &release);
  controller.brushQueue().flush();
  QApplication::processEvents();

  require(bar->value() == scrollBefore, "deferred checkbox refresh must preserve the viewport");
  if (hasCaret) {
    require(controller.selection().cursorPosition().text.sourceOffset == caretBefore,
            "checkbox click should preserve the text caret");
  }
  QString expected = original;
  expected.replace(QStringLiteral("- [ ] Complete export workflows"),
                   QStringLiteral("- [x] Complete export workflows"));
  require(session.markdownText().toString() == expected, "click should only toggle the task marker");
  const BlockLayout* updatedItem = view.blockLayoutForNode(listItemAt(session, 40, 5)->id());
  require(updatedItem && updatedItem->taskChecked(), "checkbox should repaint as checked");

  if (hasCaret && !typewriterMode) {
    controller.setCursorForSourceOffset(caretBefore);
    require(bar->value() < view.viewport()->height(), "explicit caret movement should still scroll to it");
  }
}

} // namespace

void testCheckboxClickPreservesViewportWithOffscreenCaret() {
  verifyCheckboxClickPreservesViewport(true);
}

void testCheckboxClickPreservesViewportWithoutCaret() {
  verifyCheckboxClickPreservesViewport(false);
}

void testCheckboxClickPreservesViewportInTypewriterMode() {
  verifyCheckboxClickPreservesViewport(true, true);
}

void testCheckboxClickPreservesViewportAfterOpeningOtherDocuments() {
  verifyCheckboxClickPreservesViewport(true, false, true);
}

int main(int argc, char** argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
#define RUN_TEST(test) runTest(#test, test)
  RUN_TEST(testUncheckedTaskItemRendersAsCheckbox);
  RUN_TEST(testCheckboxAlignsWithFirstLine);
  RUN_TEST(testCheckboxClickTarget);
  RUN_TEST(testCheckboxClickPreservesViewportWithOffscreenCaret);
  RUN_TEST(testCheckboxClickPreservesViewportWithoutCaret);
  RUN_TEST(testCheckboxClickPreservesViewportInTypewriterMode);
  RUN_TEST(testCheckboxClickPreservesViewportAfterOpeningOtherDocuments);
#undef RUN_TEST
  return 0;
}
