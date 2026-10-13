#include "EditorViewTestUtils.h"
#include "render/DocumentLayout.h"
#include "theme/CssThemeMapper.h"

#include <QElapsedTimer>
#include <QScrollBar>

namespace {
RenderTheme resizeTheme() {
  return RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      QStringLiteral("#write { max-width:900px; padding:20px } p { font-size:16px; margin:12px 0 } "
                     "@media (max-width:600px) { p { font-size:20px; margin:18px 0 } }"),
      QStringLiteral("resize-test"), {}));
}

void drainEstimates(DocumentLayout& layout, const RenderTheme& theme) {
  int slices = 0;
  while (layout.hasPendingViewportEstimates()) {
    layout.refreshViewportEstimates(theme, 64);
    require(++slices < 10000, "viewport estimates must finish");
  }
}

void testLazyResizeRetainsIndicesAndMatchesFreshLayout() {
  DocumentSession session;
  QString markdown;
  for (int i = 0; i < 400; ++i) {
    markdown += QStringLiteral("Paragraph %1 **bold** with wrapping words and a soft\nbreak. ").arg(i).repeated(3) + "\n\n";
    if (i % 50 == 0) markdown += "- nested **list** item with words\n  - child item\n\n```cpp\nint value = 42;\n```\n\n";
  }
  session.setMarkdownText(markdown, false);
  auto theme = resizeTheme();
  theme.updateForViewport(1000, 700);
  DocumentLayout layout;
  layout.rebuild(session.document(), theme, 1000, {}, {}, DocumentLayout::BuildPolicy::Lazy);
  layout.ensureBuilt(0, 10, theme);
  const auto& children = session.document().root().children();
  const NodeId distantId = children.back()->id();
  const qsizetype distantIndex = layout.topLevelIndexFor(distantId);
  const NodeId nestedId = children[1]->children().front()->id();
  require(layout.blockIfPromoted(nestedId), "nested list should initially be promoted");

  // Repeat before the previous idle pass finishes: only the newest width wins.
  for (const int width : {720, 480, 650, 500}) {
    const bool stylesChanged = theme.updateForViewport(width, 700);
    require(layout.relayoutForViewportWidth(theme, width, stylesChanged), "lazy resize should retain document indices");
    require(layout.hasPendingViewportEstimates(), "offscreen height work must be deferred");
    require(layout.topLevelIndexFor(distantId) == distantIndex, "resize must preserve distant node lookup");
    require(!layout.blockIfPromoted(nestedId), "stale nested geometry must not escape");
    layout.ensureBuilt(0, 10, theme);
    require(layout.blockIfPromoted(nestedId), "nested geometry must be rebuilt on demand");
    require(!layout.blockIfPromoted(distantId), "resize must not materialize distant blocks");
    layout.refreshViewportEstimates(theme, 1);
    require(layout.hasPendingViewportEstimates(), "one idle slice must not process the whole document");
  }
  drainEstimates(layout, theme);
  layout.buildAll(theme);
  DocumentLayout fresh;
  fresh.rebuild(session.document(), theme, 500);
  require(qAbs(layout.totalHeight() - fresh.totalHeight()) < .01, "resized height must match a fresh layout");
  for (const auto& child : children) {
    const auto* resized = layout.block(child->id());
    const auto* expected = fresh.block(child->id());
    require(resized && expected, "all built blocks must remain indexed");
    require(qAbs(resized->rect().top() - expected->rect().top()) < .01 &&
                qAbs(resized->rect().left() - expected->rect().left()) < .01 &&
                qAbs(resized->height() - expected->height()) < .01,
            QStringLiteral("resized block geometry differs at index %1").arg(layout.topLevelIndexFor(child->id())));
  }
  const auto* block = layout.block(nestedId, theme);
  const QPointF point = block->hitForCursorPosition(inlineCursor(nestedId, 3, 3), theme).cursorRect.center();
  require(layout.hitTest(point, theme).blockId == nestedId, "nested hit testing must use resized geometry");
}

void testCenteringIsAppliedOnDemand() {
  DocumentSession session;
  session.setMarkdownText("first\n\n- nested item\n\nlast", false);
  auto theme = resizeTheme();
  theme.updateForViewport(1200, 700);
  DocumentLayout layout;
  layout.rebuild(session.document(), theme, 1200, {}, {}, DocumentLayout::BuildPolicy::Lazy);
  layout.buildAll(theme);
  const auto id = session.document().root().children()[1]->children().front()->id();
  const qreal before = layout.block(id)->rect().left();
  const bool changed = theme.updateForViewport(1400, 700);
  require(layout.relayoutForViewportWidth(theme, 1400, changed), "centering should reuse layouts");
  require(!layout.hasPendingViewportEstimates(), "centering needs no height estimation");
  require(qAbs(layout.block(id)->rect().left() - before - 100) < .01, "nested geometry must translate exactly once");
  require(qAbs(layout.block(id)->rect().left() - before - 100) < .01, "repeated lookup must not translate twice");
}

void testHtmlViewportUnitsAndEditsDuringResize() {
  DocumentSession session;
  session.setMarkdownText("first paragraph\n\n<div style=\"height:20vh\">HTML</div>\n\nlast paragraph", false);
  auto theme = resizeTheme();
  theme.updateForViewport(1200, 700);
  DocumentLayout layout;
  layout.rebuild(session.document(), theme, 1200, {}, {}, DocumentLayout::BuildPolicy::Lazy);
  layout.buildAll(theme);
  const auto html = session.document().root().children()[1]->id();
  const qreal before = layout.block(html)->height();
  const bool changed = theme.updateForViewport(1200, 1000);
  require(layout.relayoutForViewportWidth(theme, 1200, changed), "HTML viewport change should be incremental");
  require(layout.block(html, theme)->height() > before + 40, "height-only resize must update HTML vh units");
  const auto first = session.document().root().children().front()->id();
  require(session.applyTextDelta(5, 0, "\n\nnew paragraph\n\n", true, {{first, 5, BlockType::Paragraph}}), "edit during resize");
  const auto range = session.lastLocalTopLevelRangeChange();
  require(range.isValid() && layout.rebuildTopLevelRange(range, session.document(), theme, {}).rebuilt,
          "structural edits must work with unfinished viewport estimates");
  drainEstimates(layout, theme);
  layout.buildAll(theme);
  DocumentLayout fresh;
  fresh.rebuild(session.document(), theme, 1200);
  require(qAbs(layout.totalHeight() - fresh.totalHeight()) < .01, "edited resize geometry must match fresh layout");
}

void testResizeKeepsReadingPositionWithOffscreenCaret() {
  DocumentSession session;
  session.setMarkdownText(QStringLiteral("a paragraph with enough words to wrap at a narrow width. ").repeated(4)
                              .append("\n\n").repeated(80), false);
  EditorView view;
  view.resize(700, 400);
  view.setDocument(session.document());
  view.show();
  QApplication::processEvents();
  const auto& children = session.document().root().children();
  view.setCursorPosition(inlineCursor(children.front()->id(), 0, 0));
  const NodeId anchor = children[40]->id();
  view.verticalScrollBar()->setValue(qRound(view.nodeRect(anchor).top()));
  const qreal before = view.nodeRect(anchor).top() - view.verticalScrollBar()->value();
  view.resize(480, 520);
  QApplication::processEvents();
  const qreal after = view.nodeRect(anchor).top() - view.verticalScrollBar()->value();
  require(qAbs(after - before) < 1, "resize must keep the reading block anchored, even with the caret at the start");
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < 300) QApplication::processEvents();
  require(qAbs(view.nodeRect(anchor).top() - view.verticalScrollBar()->value() - before) < 1,
          "idle height estimation must preserve the same reading anchor");
}

void benchmarkLargeResize() {
  if (!qEnvironmentVariableIsSet("MUFFIN_RESIZE_BENCH")) return;
  DocumentSession session;
  QString markdown;
  for (int i = 0; i < 50000; ++i) markdown += QStringLiteral("Paragraph %1 repeated words for wrapping. ").arg(i).repeated(4) + "\n\n";
  session.setMarkdownText(markdown, false);
  auto theme = resizeTheme();
  theme.updateForViewport(1000, 700);
  DocumentLayout layout;
  layout.rebuild(session.document(), theme, 1000, {}, {}, DocumentLayout::BuildPolicy::Lazy);
  QElapsedTimer timer;
  timer.start();
  const bool fullRebuild = qEnvironmentVariableIsSet("MUFFIN_RESIZE_BENCH_FULL");
  for (int i = 0; i < 24; ++i) {
    const int width = 500 + (i % 7) * 50;
    const bool changed = theme.updateForViewport(width, 700);
    if (fullRebuild) layout.rebuild(session.document(), theme, width, {}, {}, DocumentLayout::BuildPolicy::Lazy);
    else require(layout.relayoutForViewportWidth(theme, width, changed), "benchmark must use incremental resize");
    layout.visibleBlocks({0, 0, qreal(width), 700}, theme);
  }
  std::cout << "[resize-bench] mode=" << (fullRebuild ? "full" : "incremental") << " blocks=" << layout.slotCount()
            << " frames=24 totalMs=" << timer.elapsed() << '\n';
}
}  // namespace

int main(int argc, char** argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  runTest("lazy resize matches fresh layout", testLazyResizeRetainsIndicesAndMatchesFreshLayout);
  runTest("centering on demand", testCenteringIsAppliedOnDemand);
  runTest("HTML viewport and edits", testHtmlViewportUnitsAndEditsDuringResize);
  runTest("resize reading anchor", testResizeKeepsReadingPositionWithOffscreenCaret);
  benchmarkLargeResize();
  return 0;
}
