#include "document/DocumentSession.h"
#include "document/MarkdownNode.h"
#include "editor/EditorController.h"
#include "editor/EditorView.h"
#include "editor/SelectionController.h"
#include "render/DocumentLayout.h"
#include "theme/RenderTheme.h"

#include "EditorTestUtils.h"

#include <QApplication>
#include <QClipboard>

#include <iostream>

using namespace muffin;

namespace {
MarkdownNode* findEmptyParagraph(MarkdownNode* node) {
  if (!node)
    return nullptr;
  if (node->type() == BlockType::Paragraph) {
    const SourceRange r = node->sourceRange();
    if (r.byteStart == r.byteEnd)
      return node;
  }
  MarkdownNode* found = nullptr;
  for (const auto& c : node->children()) {
    if (MarkdownNode* f = findEmptyParagraph(c.get()))
      found = f;
  }
  return found;
}

bool rootHasOnlyBlockType(const DocumentSession& session, BlockType type) {
  const auto& kids = session.document().root().children();
  return kids.size() == 1 && kids.at(0)->type() == type;
}

struct Harness {
  DocumentSession session;
  EditorController controller;
  EditorView view;
  Harness() {
    controller.attach(&session, &view);
    view.resize(720, 460);
  }
  void load(const QString& md) {
    session.setMarkdownText(md, false);
    view.setDocument(session.document());
  }
  void placeInEmptyParagraph() {
    MarkdownNode* empty = findEmptyParagraph(&session.document().root());
    require(empty != nullptr, "document should contain an empty paragraph");
    setCursor(controller.selection(), empty, 0);
  }
  void placeInTrailing() {
    HitTestResult hit;
    hit.zone = HitTestResult::Zone::BlockAfter;
    hit.blockId = session.document().root().children().back()->id();
    hit.textNodeId = hit.blockId;
    controller.activateHit(hit);
  }
  void checkEditableLastTableCell() {
    const auto cursor = controller.selection().cursorPosition();
    const auto* table = session.document().node(cursor.blockId);
    const auto* cell = session.document().node(cursor.text.nodeId);
    require(table && table->type() == BlockType::Table && cell && cell->type() == BlockType::TableCell &&
                cell == table->children().back()->children().back().get(),
            "caret must identify the rendered table and its last editable cell");
    const auto hit = view.hitForCursorPosition(cursor);
    require(hit.zone == HitTestResult::Zone::TableCell && hit.tableRow == 1 && hit.tableColumn == 1 &&
                !hit.cursorRect.isEmpty(),
            "retreated table caret must have valid cell geometry");
    const auto original = session.markdownText().toString();
    QString typed = original;
    typed.insert(original.indexOf(QStringLiteral("2 |")) + 1, QLatin1Char('!'));
    require(controller.inputController().insertText(QStringLiteral("!")) && session.markdownText().toString() == typed,
            "typing after retreat must edit only the last cell and preserve table delimiters");
    controller.undo();
    require(session.markdownText().toString() == original, "undo must restore the table after typing in the last cell");
  }
};
} // namespace

// Table corruption guard: backspace on an empty paragraph after a table must NOT eat the
// table's closing pipes. Previously the empty paragraph merged into a table cell, truncating
// the source to "| 1 | 2" and breaking the table. The empty paragraph is removed and the caret
// lands in the last cell.
void testEmptyParagraphAfterTable() {
  Harness h;
  const QString table = QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |");
  h.load(table + QStringLiteral("\n\n"));
  h.placeInEmptyParagraph();

  require(h.controller.inputController().deleteBackward(), "backspace should be handled");
  require(h.session.markdownText().toString() == table, "table source must be intact (no corruption)");
  require(rootHasOnlyBlockType(h.session, BlockType::Table), "empty paragraph after table should be removed");

  const CursorPosition c = h.controller.selection().cursorPosition();
  require(!c.afterBlock, "caret should leave the empty paragraph");
  h.checkEditableLastTableCell();
}

// Virtual trailing caret below a table retreats into the last cell (was a stuck no-op).
void testTrailingCaretAfterTable() {
  Harness h;
  const QString table = QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |");
  h.load(table);
  h.placeInTrailing();
  require(h.controller.selection().cursorPosition().afterBlock, "caret should start on the trailing paragraph");

  require(h.controller.inputController().deleteBackward(), "backspace should be handled");
  require(h.session.markdownText().toString() == table, "trailing backspace must not change the table source");
  const CursorPosition c = h.controller.selection().cursorPosition();
  require(!c.afterBlock, "caret should leave the trailing paragraph");
  h.checkEditableLastTableCell();
}

// Mid-document regression: an empty paragraph sitting BETWEEN a table and a following block.
// Previously the mid-document case fell through to the generic paragraph-merge, which treated the
// table's last cell as the "previous editable text block" and deleted up to the empty paragraph —
// eating the table's closing pipes. Now the empty paragraph is removed and a clean "\n\n" separator
// is kept, so the table and the following block stay distinct; the caret lands in the last cell.
void testEmptyParagraphBetweenTableAndNextBlock() {
  Harness h;
  const QString table = QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |");
  const QString following = QStringLiteral("after");
  h.load(table + QStringLiteral("\n\n") + following); // [Table, Paragraph("after")]

  const auto& kids0 = h.session.document().root().children();
  require(kids0.size() == 2 && kids0.at(0)->type() == BlockType::Table && kids0.at(1)->type() == BlockType::Paragraph,
          "setup should be [Table, Paragraph]");

  // Create the mid-document empty paragraph by splitting the following paragraph at its start
  // (Enter there inserts a fresh empty paragraph before it).
  setCursor(h.controller.selection(), kids0.at(1).get(), 0);
  require(h.controller.inputController().insertParagraphBreak(), "Enter should split the paragraph");
  MarkdownNode* empty = findEmptyParagraph(&h.session.document().root());
  require(empty != nullptr && empty->previousSibling() != nullptr &&
              empty->previousSibling()->type() == BlockType::Table && empty->nextSibling() != nullptr,
          "Enter should create a mid-document empty paragraph after the table");
  setCursor(h.controller.selection(), empty, 0);

  require(h.controller.inputController().deleteBackward(), "backspace should be handled");

  // The table's closing pipes must survive, the following block must survive, and the empty
  // paragraph must be gone — leaving exactly [Table, Paragraph].
  const QString result = h.session.markdownText().toString();
  require(result.contains(QStringLiteral("| 1 | 2 |")), "table closing pipes must be intact (no corruption)");
  require(result.contains(following), "following block must survive");
  const auto& kids = h.session.document().root().children();
  require(kids.size() == 2 && kids.at(0)->type() == BlockType::Table && kids.at(1)->type() == BlockType::Paragraph,
          "empty paragraph should be removed, leaving [Table, Paragraph]");

  const CursorPosition c = h.controller.selection().cursorPosition();
  require(!c.afterBlock, "caret should leave the empty paragraph");
  h.checkEditableLastTableCell();
}

// Empty paragraph after an HTML block: deleted, caret at the block's content end (was a no-op).
void testEmptyParagraphAfterHtmlBlock() {
  Harness h;
  const QString html = QStringLiteral("<div>\nhtml\n</div>");
  h.load(html + QStringLiteral("\n\n"));
  h.placeInEmptyParagraph();

  require(h.controller.inputController().deleteBackward(), "backspace should be handled");
  require(rootHasOnlyBlockType(h.session, BlockType::HtmlBlock), "empty paragraph after HTML block should be removed");
  require(h.session.markdownText().toString().contains(html), "html source must be intact");
  const CursorPosition c = h.controller.selection().cursorPosition();
  require(!c.afterBlock, "caret should leave the empty paragraph");
  MarkdownNode* block = h.session.document().node(c.blockId);
  require(block != nullptr && block->type() == BlockType::HtmlBlock, "caret should retreat into the HTML block");
}

void testEmptyParagraphBetweenListAndNextBlock() {
  const QString lists[] = {
      "* 1\n* 2\n* 3\n* 4\n* 5",
      "- 1\n- 2\n- 3\n- 4\n- 5",
      "+ 1\n+ 2\n+ 3\n+ 4\n+ 5",
      "1. 1\n2. 2\n3. 3\n4. 4\n5. 5",
      "9) 1\n10) 2\n11) 3\n12) 4\n13) 5",
      "- [ ] 1\n- [x] 2\n- [ ] 3\n- [x] 4\n- [ ] 5",
      "- 1\n- 2\n- 3\n- 4\n- parent\n  - 5",
      "1. 1\n2. 2\n3. 3\n4. 4\n5. parent\n   1. 5",
  };
  for (const QString& list : lists) {
    Harness h;
    const QString original = list + QStringLiteral("\n\n123");
    h.load(original);
    auto* following = h.session.document().root().children().back().get();
    require(following->type() == BlockType::Paragraph, "fixture should end in a plain paragraph");
    setCursor(h.controller.selection(), following, 0);
    require(h.controller.inputController().insertParagraphBreak(), "Enter should create an empty paragraph");
    const QString withEmptyParagraph = h.session.markdownText().toString();
    h.placeInEmptyParagraph();
    require(h.controller.inputController().deleteBackward(), "Backspace after a list should be handled");
    require(h.session.markdownText().toString() == original,
            QStringLiteral("Backspace must preserve the entire preceding list: %1 -> %2")
                .arg(original, h.session.markdownText().toString()));
    const auto& blocks = h.session.document().root().children();
    require(blocks.size() == 2 && blocks.front()->type() == BlockType::List && blocks.front()->children().size() == 5 &&
                blocks.back()->type() == BlockType::Paragraph,
            "deleting a separator must preserve the list in the parsed document");
    const auto cursor = h.controller.selection().cursorPosition();
    const MarkdownNode* block = h.session.document().node(cursor.blockId);
    require(block && !cursor.afterBlock && block->parent() && block->parent()->type() == BlockType::List,
            "Backspace should retreat into the final list item");
    require(cursor.text.textOffset == 1, "caret should land after the final list item's text");
    require(h.controller.inputController().insertText(QStringLiteral("!")),
            "the restored caret should remain editable");
    require(h.session.markdownText().toString() == list + QStringLiteral("!\n\n123"),
            "typing after Backspace should edit the last list item rather than the following paragraph");
    h.controller.undo();
    h.controller.undo();
    require(h.session.markdownText().toString() == withEmptyParagraph &&
                findEmptyParagraph(&h.session.document().root()) != nullptr,
            "undo should restore the empty paragraph after the list");
    h.controller.redo();
    require(h.session.markdownText().toString() == original &&
                h.session.document().root().children().front()->type() == BlockType::List,
            "redo should remove the empty paragraph and preserve the list");
  }
}

void testEmptyParagraphAfterContainerWithLiteralTail() {
  const QString containers[] = {
      "- item\n\n  ```txt\n  code\n  ```",
      "1. item\n\n   ```txt\n   code\n   ```",
      "> ```txt\n> code\n> ```",
  };
  for (const QString& before : containers) {
    Harness h;
    const QString original = before + QStringLiteral("\n\n123");
    h.load(original);
    setCursor(h.controller.selection(), h.session.document().root().children().back().get(), 0);
    require(h.controller.inputController().insertParagraphBreak(),
            "Enter should create a blank paragraph after the container");
    const QString withBlank = h.session.markdownText().toString();
    h.placeInEmptyParagraph();
    require(h.controller.inputController().deleteBackward(), "Backspace after a container should be handled");
    require(h.session.markdownText().toString() == original,
            "removing an empty paragraph must preserve literal blocks inside the preceding container");
    const auto cursor = h.controller.selection().cursorPosition();
    const auto* block = h.session.document().node(cursor.blockId);
    require(block && block->type() == BlockType::CodeFence && !cursor.afterBlock,
            "caret should retreat into the container's last literal block");
    h.controller.undo();
    require(h.session.markdownText().toString() == withBlank, "undo must restore the blank paragraph");
    h.controller.redo();
    require(h.session.markdownText().toString() == original, "redo must preserve the container's literal content");
  }
}

// Front matter's trailing-newline-sensitive parsing makes deleting a trailing empty paragraph
// unsafe (the closing fence needs its newline). Backspace there must be a safe no-op: document
// intact, caret valid — never corrupted or dropped.
void testEmptyParagraphAfterFrontMatterIsSafe() {
  Harness h;
  const QString fm = QStringLiteral("---\nkey: value\n---");
  h.load(fm + QStringLiteral("\n\n"));
  h.placeInEmptyParagraph();
  h.controller.inputController().deleteBackward();
  require(h.session.markdownText().toString().contains(fm), "front matter must survive backspace");
  require(h.controller.selection().hasCursor(), "caret should remain valid after backspace near front matter");
}

// The previously-working cases must keep working (regression guard for the common blocks).
void testEmptyParagraphAfterCommonBlocks() {
  struct Case {
    const char* name;
    QString source;
  };
  const Case cases[] = {
      {"paragraph", QStringLiteral("alpha")},
      {"heading", QStringLiteral("## Title")},
      {"ordered-list", QStringLiteral("1. first\n2. second")},
      {"unordered-list", QStringLiteral("- first\n- second")},
      {"code-fence", QStringLiteral("```\ncode line\n```")},
      {"math-block", QStringLiteral("$$\nx^2\n$$")},
      {"block-quote", QStringLiteral("> quote line")},
  };
  for (const auto& k : cases) {
    Harness h;
    h.load(k.source + QStringLiteral("\n\n"));
    h.placeInEmptyParagraph();
    require(h.controller.inputController().deleteBackward(), "backspace should be handled");
    require(h.session.markdownText().toString() == k.source, "empty paragraph after common block should be removed");
    require(h.controller.selection().cursorPosition().isValid(), "caret should remain valid");
  }
}

// A LEADING thematic break (no editable content before it) followed by an empty paragraph:
// backspace cannot eat the divider because there is nowhere to retreat the caret, so it stays a
// safe no-op (document unchanged, caret valid) — it must never corrupt or drop the caret. (When
// there IS editable content before the rule, backspace instead removes the divider — see
// ThematicBreakEditTest.)
void testEmptyParagraphAfterThematicBreakIsSafe() {
  Harness h;
  const QString md = QStringLiteral("---\n\n");
  h.load(md);
  h.placeInEmptyParagraph();
  h.controller.inputController().deleteBackward();
  require(h.controller.selection().hasCursor(), "caret should remain valid after backspace near a thematic break");
  // Document must not be corrupted (the table bug class): still contains the thematic break.
  require(h.session.markdownText().toString().contains(QStringLiteral("---")), "thematic break must survive backspace");
}

// Regression: typing "---" (→ thematic break), then clicking the virtual trailing area below it
// and typing text, must NOT leave a phantom paragraph that renders the thematic break's source
// "---" as literal text. Root cause: the incremental local-edit slice parse synthesized a
// context-dependent virtual empty paragraph at the slice boundary, which then rendered the
// preceding thematic break's line range as "---". The fix strips boundary VEPs from slice results.
void testTypingAfterThematicBreakRendersNoStaleMarker() {
  Harness h;
  h.load(QString());
  // Type "---" character by character; the third dash commits it to a thematic break.
  h.controller.inputController().insertText(QStringLiteral("-"));
  h.controller.inputController().insertText(QStringLiteral("-"));
  h.controller.inputController().insertText(QStringLiteral("-"));
  require(h.session.document().root().children().size() == 1, "typed '---' should be a single thematic break");
  require(h.session.document().root().children().at(0)->type() == BlockType::ThematicBreak,
          "typed '---' should parse as a thematic break");

  // Click the virtual trailing paragraph below the break and type "123".
  h.placeInTrailing();
  h.controller.inputController().insertText(QStringLiteral("123"));

  // Source must be exactly the break followed by the typed paragraph.
  require(h.session.markdownText().toString() == QStringLiteral("---\n\n123"), "markdown should be '---\\n\\n123'");

  // The tree must hold only the thematic break and the new paragraph — no phantom in between.
  const auto& kids = h.session.document().root().children();
  require(kids.size() == 2, "should be exactly 2 blocks (break + paragraph), no phantom");
  require(kids.at(0)->type() == BlockType::ThematicBreak, "first block should remain the thematic break");
  require(kids.at(1)->type() == BlockType::Paragraph, "second block should be the typed paragraph");

  // The user-visible symptom: no rendered block may display the stale "---" source.
  DocumentLayout layout;
  layout.rebuild(h.session.document(), RenderTheme::github(), 900.0, h.controller.selection().selection());
  for (const auto& c : kids) {
    const BlockLayout* bl = layout.block(c->id());
    const QString display = (bl && bl->inlineLayout()) ? bl->inlineLayout()->displayText() : QString();
    require(display != QStringLiteral("---"), "no block should render the stale '---' marker text");
  }
}

// Same guard via the loaded-break path: a pre-existing thematic break followed by typing into its
// trailing area must not resurrect the "---" as text either. This exercises the slice parse with a
// leading blank line directly.
void testTypingAfterLoadedThematicBreakRendersNoStaleMarker() {
  Harness h;
  h.load(QStringLiteral("---"));
  require(h.session.document().root().children().at(0)->type() == BlockType::ThematicBreak,
          "loaded '---' should be a thematic break");
  h.placeInTrailing();
  h.controller.inputController().insertText(QStringLiteral("xyz"));

  require(h.session.markdownText().toString() == QStringLiteral("---\n\nxyz"), "markdown should be '---\\n\\nxyz'");
  require(h.session.document().root().children().size() == 2, "should be exactly 2 blocks, no phantom");

  DocumentLayout layout;
  layout.rebuild(h.session.document(), RenderTheme::github(), 900.0, h.controller.selection().selection());
  for (const auto& c : h.session.document().root().children()) {
    const BlockLayout* bl = layout.block(c->id());
    const QString display = (bl && bl->inlineLayout()) ? bl->inlineLayout()->displayText() : QString();
    require(display != QStringLiteral("---"), "no block should render the stale '---' marker text");
  }
}

int main(int argc, char** argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  testEmptyParagraphAfterTable();
  testEmptyParagraphBetweenTableAndNextBlock();
  testEmptyParagraphBetweenListAndNextBlock();
  testEmptyParagraphAfterContainerWithLiteralTail();
  testTrailingCaretAfterTable();
  testEmptyParagraphAfterHtmlBlock();
  testEmptyParagraphAfterFrontMatterIsSafe();
  testEmptyParagraphAfterCommonBlocks();
  testEmptyParagraphAfterThematicBreakIsSafe();
  testTypingAfterThematicBreakRendersNoStaleMarker();
  testTypingAfterLoadedThematicBreakRendersNoStaleMarker();
  QApplication::clipboard()->clear();
  return 0;
}
