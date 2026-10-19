#include "app/RenderEditorBackend.h"
#include "document/DocumentSession.h"
#include "document/MarkdownNode.h"
#include "editor/EditorController.h"
#include "editor/EditorView.h"
#include "editor/SelectionController.h"

#include "EditorTestUtils.h"

#include <QApplication>
#include <QClipboard>
#include <QMimeData>

#include <iostream>

using namespace muffin;

namespace {

struct Harness {
  DocumentSession session;
  EditorController controller;
  EditorView view;
  RenderEditorBackend backend;
  Harness() : backend(controller, session, &view) {
    controller.attach(&session, &view);
    view.resize(720, 460);
  }
  void load(const QString& md) {
    session.setMarkdownText(md, false);
    view.setDocument(session.document());
  }
  // Place the caret at `offset` inside the block that resolves to `block`
  // (e.g. a heading, paragraph, or list-item content node).
  void placeIn(MarkdownNode* block, qsizetype offset) {
    setCursor(controller.selection(), block, offset);
  }
  void placeInTrailing() {
    HitTestResult hit;
    hit.zone = HitTestResult::Zone::BlockAfter;
    hit.blockId = session.document().root().children().back()->id();
    hit.textNodeId = hit.blockId;
    controller.activateHit(hit);
  }
};

}  // namespace

// deleteRange(Word) removes the word at the caret, leaving surrounding text.
void testDeleteWordInParagraph() {
  Harness h;
  h.load(QStringLiteral("hello world"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 6);  // caret at start of "world"
  h.backend.deleteRange(DeleteTarget::Word);
  require(h.session.markdownText().toString() == QStringLiteral("hello "), "delete word should remove \"world\"");
}

// A caret in the middle of a word still selects (and deletes) the whole word.
void testDeleteWordMidWord() {
  Harness h;
  h.load(QStringLiteral("hello world"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 8);  // caret inside "world"
  h.backend.deleteRange(DeleteTarget::Word);
  require(h.session.markdownText().toString() == QStringLiteral("hello "), "delete word from mid-word should remove the whole word");
}

// deleteRange(Word) on a bare caret between words is a safe no-op.
void testDeleteWordNoWordAtCursor() {
  Harness h;
  h.load(QStringLiteral("hello   world"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 6);  // caret in the run of spaces
  h.backend.deleteRange(DeleteTarget::Word);
  require(h.session.markdownText().toString() == QStringLiteral("hello   world"), "delete word on whitespace should be a no-op");
}

// deleteRange(FormatSpan) removes the inline-format span's visible text.
void testDeleteFormatSpanBold() {
  Harness h;
  h.load(QStringLiteral("a **bold** b"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 5);  // caret inside "bold"
  h.backend.deleteRange(DeleteTarget::FormatSpan);
  require(!h.session.markdownText().toString().contains(QStringLiteral("bold")), "delete format span should remove the bold text");
}

// deleteRange(Line) clears a heading's content but keeps the marker ("## ").
void testDeleteLineHeadingKeepsMarker() {
  Harness h;
  h.load(QStringLiteral("## Title"));
  MarkdownNode* heading = blockAt(h.session, 0);
  h.placeIn(heading, 2);  // caret inside the title text
  h.backend.deleteRange(DeleteTarget::Line);
  require(h.session.markdownText().toString() == QStringLiteral("## "), "delete line on a heading should keep the \"## \" marker");
  require(h.session.document().root().children().size() == 1, "heading block should still exist");
}

// deleteRange(Line) on a markerless paragraph removes the block (an empty
// paragraph has no content to preserve, so it collapses — same as Block).
void testDeleteLineParagraphRemovesBlock() {
  Harness h;
  h.load(QStringLiteral("alpha\n\nbeta"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 1);
  h.backend.deleteRange(DeleteTarget::Line);
  require(h.session.markdownText().toString() == QStringLiteral("beta"), "delete line on a paragraph should remove the block");
}

// deleteRange(Block) removes a whole heading block and joins neighbours.
void testDeleteBlockHeading() {
  Harness h;
  h.load(QStringLiteral("## Title\n\nbody"));
  MarkdownNode* heading = blockAt(h.session, 0);
  h.placeIn(heading, 2);
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.session.markdownText().toString() == QStringLiteral("body"), "delete block should remove the heading and its separators");
  require(h.session.document().root().children().size() == 1, "one block should remain");
}

// deleteRange(Block) on the only block leaves a valid empty document.
void testDeleteBlockOnlyBlock() {
  Harness h;
  h.load(QStringLiteral("alpha"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 1);
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.session.markdownText().toString().isEmpty(), "deleting the only block should empty the document");
  require(!h.session.markdownText().toString().contains(QLatin1String("alpha")), "no block content should remain");
}

// deleteRange(Block) removes a code fence entirely.
void testDeleteBlockCodeFence() {
  Harness h;
  h.load(QStringLiteral("```cpp\ncode\n```"));
  MarkdownNode* code = blockAt(h.session, 0);
  require(code->type() == BlockType::CodeFence, "first block should be a code fence");
  h.placeIn(code, 2);
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.session.markdownText().toString().isEmpty(), "deleting the code fence should empty the document");
}

// deleteRange(Block) inside a list removes the entire list (v1: list-item-level
// removal is not supported; the top-level List node is removed).
void testDeleteBlockListRemovesWholeList() {
  Harness h;
  h.load(QStringLiteral("- one\n- two"));
  MarkdownNode* list = blockAt(h.session, 0);
  require(list->type() == BlockType::List, "first block should be a list");
  // Caret in the first item's content.
  MarkdownNode* item = listItemAt(h.session, 0, 0);
  MarkdownNode* itemPara = firstChildOfType(item, BlockType::Paragraph);
  h.placeIn(itemPara, 1);
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.session.markdownText().toString().isEmpty(), "delete block in a list should remove the whole list");
}

// deleteRange(Block) on the trailing caret is a no-op (must not delete the last block).
void testDeleteBlockTrailingCaretIsNoOp() {
  Harness h;
  h.load(QStringLiteral("alpha"));
  h.placeInTrailing();
  require(h.controller.selection().cursorPosition().afterBlock, "caret should be on the trailing paragraph");
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.session.markdownText().toString() == QStringLiteral("alpha"), "delete block from trailing caret must not change the document");
}

// deleteRange(Block) produces a single undoable RemoveNodeCommand; redo re-applies.
void testDeleteBlockUndoRedo() {
  Harness h;
  h.load(QStringLiteral("## Title\n\nbody"));
  MarkdownNode* heading = blockAt(h.session, 0);
  h.placeIn(heading, 2);
  const QString before = h.session.markdownText().toString();

  h.backend.deleteRange(DeleteTarget::Block);
  require(h.session.markdownText().toString() == QStringLiteral("body"), "block should be removed");

  require(h.controller.canUndo(), "delete block should be undoable");
  h.controller.undo();
  require(h.session.markdownText().toString() == before, "undo should restore the deleted block");

  require(h.controller.canRedo(), "redo should be available after undo");
  h.controller.redo();
  require(h.session.markdownText().toString() == QStringLiteral("body"), "redo should re-remove the block");
}

// deleteRange(Forward) preserves the legacy "delete selection else one char" semantics.
void testDeleteRangeForwardCollapsesSelection() {
  Harness h;
  h.load(QStringLiteral("alpha"));
  MarkdownNode* para = blockAt(h.session, 0);
  h.placeIn(para, 0);
  h.backend.deleteRange(DeleteTarget::Forward);
  require(h.session.markdownText().toString() == QStringLiteral("lpha"), "forward delete should remove one char");
}

void testRenderedDeletionUsesWholeGraphemes() {
  const QStringList clusters{QStringLiteral("\U0001F600"), QStringLiteral("\U00020000"),
                             QStringLiteral("e\u0301"), QStringLiteral("\U0001F469\u200D\U0001F4BB"),
                             QStringLiteral("\U0001F1E8\U0001F1F3")};
  for (const QString& prefix : {QString(), QStringLiteral("# "), QStringLiteral("- ")}) {
    for (const QString& cluster : clusters) {
      for (const bool backward : {false, true}) {
        Harness h;
        const QString before = prefix + QStringLiteral("a") + cluster + QStringLiteral("b");
        h.load(before);
        MarkdownNode* block = prefix == QStringLiteral("- ")
            ? listItemAt(h.session, 0, 0) : blockAt(h.session, 0);
        h.placeIn(block, backward ? 1 + cluster.size() : 1);
        h.backend.deleteRange(backward ? DeleteTarget::Backward : DeleteTarget::Forward);
        require(h.session.markdownText().toString() == prefix + QStringLiteral("ab"),
                "rendered delete must remove the whole Unicode grapheme");
        h.controller.undo();
        require(h.session.markdownText().toString() == before, "Unicode deletion undo must restore exact source");
        h.controller.redo();
        require(h.session.markdownText().toString() == prefix + QStringLiteral("ab"),
                "Unicode deletion redo must remain well formed");
      }
    }
  }
}

void testSelectAllDeletesMixedDocument() {
  const QString before = QString::fromUtf8(
      "* 123\n* 123\n* 123\n\n123\n\n123\n\n123\n\n"
      "- 123 **13\n- 123-123\n- 321\n\n[123]()\n\n213哇哦\n\n我有时候感觉我\n\n\n");
  Harness h;
  h.load(before);
  h.controller.setCursorForSourceOffset(before.indexOf(QString::fromUtf8("我有时候")) + 2);
  require(pressKey(h.controller.inputController(), &h.view, Qt::Key_A, Qt::ControlModifier), "Ctrl+A selects the mixed document");
  require(selectedMarkdown(h.session, h.controller.selection()) == before, "whole document copy includes Markdown markers and blank lines");
  require(pressKey(h.controller.inputController(), &h.view, Qt::Key_Backspace), "Backspace handles the whole document selection");
  require(h.session.markdownText().isEmpty(), "Ctrl+A then Backspace clears list markers, links, text and trailing blank lines");
  require(h.controller.canUndo(), "clearing the document is undoable");
  h.controller.undo();
  require(h.session.markdownText().toString() == before, "undo restores the complete original Markdown");
  h.controller.redo();
  require(h.session.markdownText().isEmpty(), "redo clears the complete document again");
  require(h.controller.inputController().insertText(QString::fromUtf8("重新输入")), "typing works after clearing the document");
  require(h.session.markdownText().toString() == QString::fromUtf8("重新输入"), "new text starts in an empty document");
}

void testSelectAllCoversStructuredDocumentEdges() {
  const QStringList documents{
      "- first\n- second\n",
      "- outer\n  - nested\n",
      "> first\n>\n> second\n",
      "\n\n## **Title**\n\n[link]()\n\n\n",
      "---\n\nbody\n\n---\n",
      "```cpp\ncode\n```\n\nbody\n\n$$\nx+y\n$$\n",
      "| A | B |\n| --- | --- |\n| one | two |\n\nbody\n",
      "body\n\n| A | B |\n| --- | --- |\n| one | two |\n",
      "![image](missing.png)\n",
      "# heading\n",
      "paragraph\n\n\n"};
  for (const auto& before : documents) {
    for (int operation = 0; operation < 3; ++operation) {
      Harness h;
      h.load(before);
      auto* initial = blockAt(h.session, 0);
      for (const auto& block : h.session.document().root().children()) {
        if (block->type() == BlockType::Paragraph) initial = block.get();
      }
      // Establish a document caret, outside literal/cell editing contexts.
      CursorPosition cursor;
      cursor.blockId = initial->id();
      cursor.text.nodeId = initial->id();
      h.controller.selection().setCursorPosition(cursor);
      require(h.controller.selectAll(), "select all handles structured document boundaries");
      require(selectedMarkdown(h.session, h.controller.selection()) == before,
              QStringLiteral("whole document selection includes all source: %1").arg(before));
      qsizetype start = -1, end = -1;
      require(h.controller.selectionSourceRange(start, end) && start == 0 && end == before.size(),
              "whole document edit range includes markers, fences and blank lines");
      if (operation == 0) h.backend.deleteRange(DeleteTarget::Backward);
      else if (operation == 1) h.backend.deleteRange(DeleteTarget::Forward);
      else require(h.controller.inputController().insertText("replacement"), "typing replaces the whole document selection");
      const QString after = operation == 2 ? QStringLiteral("replacement") : QString();
      require(h.session.markdownText().toString() == after,
              QStringLiteral("whole selection edit leaves no source fragments (operation %1): %2")
                  .arg(operation).arg(h.session.markdownText().toString()));
      h.controller.undo();
      require(h.session.markdownText().toString() == before, "one undo restores exact structured source");
      h.controller.redo();
      require(h.session.markdownText().toString() == after, "redo reapplies the whole selection edit");
    }
  }
  Harness cut;
  const QString before = "- first\n- second\n\n# end\n\n";
  cut.load(before);
  cut.placeIn(blockAt(cut.session, 1), 1);
  cut.backend.selectAll();
  cut.backend.cut();
  require(cut.session.markdownText().isEmpty() &&
              QApplication::clipboard()->mimeData()->data("text/markdown") == before.toUtf8(),
          "cut removes the whole selection and copies exact source");
  cut.controller.undo();
  require(cut.session.markdownText().toString() == before, "cut is one undoable edit");
}

int main(int argc, char** argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  testSelectAllDeletesMixedDocument();
  testSelectAllCoversStructuredDocumentEdges();
  testRenderedDeletionUsesWholeGraphemes();
  testDeleteWordInParagraph();
  testDeleteWordMidWord();
  testDeleteWordNoWordAtCursor();
  testDeleteFormatSpanBold();
  testDeleteLineHeadingKeepsMarker();
  testDeleteLineParagraphRemovesBlock();
  testDeleteBlockHeading();
  testDeleteBlockOnlyBlock();
  testDeleteBlockCodeFence();
  testDeleteBlockListRemovesWholeList();
  testDeleteBlockTrailingCaretIsNoOp();
  testDeleteBlockUndoRedo();
  testDeleteRangeForwardCollapsesSelection();
  QApplication::clipboard()->clear();
  return 0;
}
