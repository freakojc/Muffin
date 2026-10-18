#include "app/SourceEditorBackend.h"
#include "document/DocumentSession.h"
#include "editor/SourceEditorWidget.h"
#include "editor/VirtualSourceEdit.h"

#include "../TestUtils.h"

#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QKeyEvent>
#include <QScrollBar>
#include <iostream>

using namespace muffin;

namespace {

struct Harness {
  SourceEditorWidget editor;
  SourceEditorBackend backend;
  Harness() : backend(&editor) {
    editor.resize(720, 460);
  }
  void load(const QString& text) {
    editor.setText(text);
  }
  // Place the caret at a flat document position.
  void placeAt(int position) {
    editor.setCursorPosition(position);
  }
  // Select the closed interval [start, end] (end exclusive).
  void select(int start, int end) {
    editor.setSelection(start, end);
  }
  QString text() const {
    return editor.text();
  }
};

}  // namespace

void testSourceDeleteForwardChar() {
  Harness h;
  h.load(QStringLiteral("abc"));
  h.placeAt(0);
  h.backend.deleteRange(DeleteTarget::Forward);
  require(h.text() == QStringLiteral("bc"), "forward delete should remove one char");
}

void testSourceDeleteForwardSelection() {
  Harness h;
  h.load(QStringLiteral("abc"));
  h.select(0, 2);
  h.backend.deleteRange(DeleteTarget::Forward);
  require(h.text() == QStringLiteral("c"), "forward delete should remove the selection");
}

void testSourceDeleteBackwardChar() {
  Harness h;
  h.load(QStringLiteral("abc"));
  h.placeAt(2);
  h.backend.deleteRange(DeleteTarget::Backward);
  require(h.text() == QStringLiteral("ac"), "backward delete should remove the previous char");
}

void testSourceDeleteWord() {
  Harness h;
  h.load(QStringLiteral("hello world"));
  h.placeAt(7);  // inside "world"
  h.backend.deleteRange(DeleteTarget::Word);
  require(h.text() == QStringLiteral("hello "), "delete word should remove \"world\"");
}

// Source mode has no markdown-aware format notion; FormatSpan behaves as Word.
void testSourceDeleteFormatSpan() {
  Harness h;
  h.load(QStringLiteral("hello world"));
  h.placeAt(7);
  h.backend.deleteRange(DeleteTarget::FormatSpan);
  require(h.text() == QStringLiteral("hello "), "delete format span should match word in source mode");
}

// Line clears the current line's text but leaves the (empty) line in place.
void testSourceDeleteLine() {
  Harness h;
  h.load(QStringLiteral("line1\nline2\nline3"));
  h.placeAt(6);  // start of "line2"
  h.backend.deleteRange(DeleteTarget::Line);
  require(h.text() == QStringLiteral("line1\n\nline3"), "delete line should clear the line text, leaving an empty line");
}

// Block removes a middle line entirely (line text + its newline), joining neighbours.
void testSourceDeleteBlockMiddle() {
  Harness h;
  h.load(QStringLiteral("line1\nline2\nline3"));
  h.placeAt(6);  // inside "line2"
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.text() == QStringLiteral("line1\nline3"), "delete block on a middle line should remove the line and join");
}

// Block on the last line removes the line text (previous newline kept).
void testSourceDeleteBlockLast() {
  Harness h;
  h.load(QStringLiteral("line1\nline2"));
  h.placeAt(6);  // inside "line2"
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.text() == QStringLiteral("line1\n"), "delete block on the last line should leave the previous line");
}

// Block on the only line empties the document.
void testSourceDeleteBlockOnly() {
  Harness h;
  h.load(QStringLiteral("only"));
  h.placeAt(1);
  h.backend.deleteRange(DeleteTarget::Block);
  require(h.text().isEmpty(), "delete block on the only line should empty the document");
}

void testSessionBackedSourceEditsAndUndo() {
  DocumentSession session;
  session.setMarkdownText(QStringLiteral("alpha\nbeta"), false);
  SourceEditorWidget editor;
  editor.bindSession(&session);
  SourceEditorBackend backend(&editor);

  editor.setCursorPosition(QStringLiteral("alpha").size());
  editor.insertText(QStringLiteral("!"));
  require(session.markdownText().toString() == QStringLiteral("alpha!\nbeta"),
          "source edit should update the session PieceTable directly");
  require(backend.canUndo(), "session-backed source edit should be undoable");
  backend.undo();
  require(session.markdownText().toString() == QStringLiteral("alpha\nbeta"),
          "source undo should apply the inverse PieceTable delta");
  backend.redo();
  require(session.markdownText().toString() == QStringLiteral("alpha!\nbeta"),
          "source redo should reapply the PieceTable delta");

  editor.setSelection(0, 5);
  editor.replaceSelection(QStringLiteral("A"));
  require(session.markdownText().toString() == QStringLiteral("A!\nbeta"),
          "source selection replacement should update the session");
}

void testReadOnlySourceRejectsMutationWithoutDamagingUndo() {
  Harness h;
  h.load(QStringLiteral("alpha"));
  h.placeAt(5);
  h.editor.insertText(QStringLiteral("!"));
  require(h.backend.canUndo(), "fixture edit should be undoable");

  h.editor.setReadOnly(true);
  h.editor.insertText(QStringLiteral(" ignored"));
  h.backend.undo();
  require(h.text() == QStringLiteral("alpha!"),
          "read-only input and undo must leave text and undo history unchanged");

  h.editor.setReadOnly(false);
  h.backend.undo();
  require(h.text() == QStringLiteral("alpha"),
          "undo history should remain usable after read-only mode ends");
}

void testSessionBackedBackspaceInListSeparator() {
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
    for (int blankLines : {1, 2}) {
      DocumentSession session;
      const QString original = list + QString(blankLines + 1, QLatin1Char('\n')) + QStringLiteral("123");
      session.setMarkdownText(original, false);
      SourceEditorWidget editor;
      editor.bindSession(&session);
      QObject::connect(&session, &DocumentSession::documentTextChanged, &editor,
                       [&editor] { editor.syncFromSession(); });
      QObject::connect(&session, &DocumentSession::documentLocallyEdited, &editor,
                       [&editor] { editor.notifyDocumentChanged(); });
      SourceEditorBackend backend(&editor);
      VirtualSourceEdit* view = editor.findChild<VirtualSourceEdit*>();
      require(view != nullptr, "source editor should expose its keyboard target");
      const qsizetype caret = list.size() + 1;
      editor.setCursorPosition(caret);
      QKeyEvent backspace(QEvent::KeyPress, Qt::Key_Backspace, Qt::NoModifier);
      QApplication::sendEvent(view, &backspace);

      QString expected = original;
      expected.remove(caret - 1, 1);
      require(session.markdownText().toString() == expected,
              QStringLiteral("source Backspace on a blank line after a list must remove the preceding newline: blanks=%1, caret=%2, actual=[%3]")
                  .arg(blankLines).arg(editor.cursorPosition()).arg(session.markdownText().toString()));
      require(editor.cursorPosition() == caret - 1, "source Backspace must move the caret back one character");
      require(session.lastParseWasLocalEdit(), "ordinary source separator edits must not reparse the entire document");
      const auto checkTree = [&session, blankLines](qsizetype expectedBlocks, const char* stage) {
        const auto& blocks = session.document().root().children();
        require(static_cast<qsizetype>(blocks.size()) == expectedBlocks &&
                    blocks.front()->type() == BlockType::List && blocks.front()->children().size() == 5,
                QStringLiteral("source separator tree mismatch after %1 with %2 blank lines: expected=%3 actual=%4 firstType=%5 items=%6 source=[%7]")
                    .arg(QString::fromLatin1(stage)).arg(blankLines).arg(expectedBlocks).arg(blocks.size())
                    .arg(blocks.empty() ? -1 : static_cast<int>(blocks.front()->type()))
                    .arg(blocks.empty() ? 0 : blocks.front()->children().size())
                    .arg(session.markdownText().toString().replace(QLatin1Char('\n'), QStringLiteral("\\n"))));
      };
      checkTree(blankLines == 1 ? 1 : 2, "Backspace");
      require(backend.canUndo(), "deleting a list separator should be undoable");
      backend.undo();
      require(session.markdownText().toString() == original && editor.cursorPosition() == caret,
              "undo should restore the blank line and caret");
      checkTree(blankLines + 1, "undo");
      backend.redo();
      require(session.markdownText().toString() == expected && editor.cursorPosition() == caret - 1,
              "redo should delete the separator again");
      checkTree(blankLines == 1 ? 1 : 2, "redo");
    }
  }
}

void testSessionBackedSourceRejectsEditsDuringOpen() {
  DocumentSession session;
  session.setMarkdownText(QStringLiteral("alpha"), false);
  SourceEditorWidget editor;
  editor.bindSession(&session);
  editor.setCursorPosition(5);
  editor.insertText(QStringLiteral("!"));

  session.setOpenConfirmationPending(true);
  editor.deleteBackward();
  editor.insertText(QStringLiteral("ignored"));
  editor.undo();
  require(session.markdownText().toString() == QStringLiteral("alpha!") && editor.cursorPosition() == 6,
          "source edits and fallback must leave the old document untouched while an open is pending");
  session.setOpenConfirmationPending(false);
  editor.undo();
  require(session.markdownText().toString() == QStringLiteral("alpha"),
          "an edit rejected during open must preserve the source undo history");
}

void testVirtualSourceViewportAffordances() {
  SourceEditorWidget editor;
  editor.resize(720, 300);
  editor.setText(QStringLiteral("source line\n").repeated(1000));
  editor.show();
  QApplication::processEvents();

  VirtualSourceEdit* view = editor.findChild<VirtualSourceEdit*>();
  require(view != nullptr, "source widget should own a virtual source view");
  require(view->verticalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
          "large source documents should expose a vertical scrollbar");
  require(view->verticalScrollBar()->maximum() > 0 && view->verticalScrollBar()->isVisible(),
          "vertical scrollbar should be visible for overflowing source text");
  require(view->viewport()->cursor().shape() == Qt::IBeamCursor,
          "source text viewport should use an I-beam cursor");

  const QImage viewportImage = view->viewport()->grab().toImage();
  bool foundDarkBodyPixel = false;
  for (int y = 0; y < qMin(50, viewportImage.height()) && !foundDarkBodyPixel; ++y) {
    for (int x = 72; x < qMin(220, viewportImage.width()); ++x) {
      if (qGray(viewportImage.pixel(x, y)) < 130) {
        foundDarkBodyPixel = true;
        break;
      }
    }
  }
  require(foundDarkBodyPixel,
          "plain source text should use the theme body color, not the pale line-number color");

  editor.setWordWrapEnabled(false);
  require(view->horizontalScrollBarPolicy() == Qt::ScrollBarAsNeeded,
          "unwrapped source text should expose a horizontal scrollbar when needed");
}

int main(int argc, char** argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
  testSourceDeleteForwardChar();
  testSourceDeleteForwardSelection();
  testSourceDeleteBackwardChar();
  testSourceDeleteWord();
  testSourceDeleteFormatSpan();
  testSourceDeleteLine();
  testSourceDeleteBlockMiddle();
  testSourceDeleteBlockLast();
  testSourceDeleteBlockOnly();
  testSessionBackedSourceEditsAndUndo();
  testSessionBackedBackspaceInListSeparator();
  testSessionBackedSourceRejectsEditsDuringOpen();
  testReadOnlySourceRejectsMutationWithoutDamagingUndo();
  testVirtualSourceViewportAffordances();
  QApplication::clipboard()->clear();
  return 0;
}
