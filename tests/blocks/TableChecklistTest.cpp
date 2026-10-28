#include "blocks/table/TableChecklist.h"
#include "blocks/table/TableModelOps.h"
#include "document/DocumentSession.h"
#include "document/InlineNode.h"
#include "document/MarkdownNode.h"
#include "editor/EditorController.h"
#include "editor/EditorView.h"

#include "../editor/EditorTestUtils.h"

#include <QApplication>
#include <QString>

#include <cstdlib>
#include <iostream>

using namespace muffin;

namespace {

QString cellText(const MarkdownNode& cell) {
  QString result;
  for (const InlineNode& inlineNode : cell.inlines()) {
    result += inlineNode.text();
  }
  return result;
}

HitTestResult cellHitWith(const MarkdownNode& table, int row, int column) {
  HitTestResult hit;
  hit.zone = HitTestResult::Zone::TableCell;
  hit.blockId = table.id();
  const MarkdownNode* cell = TableModelOps::cellAt(table, row, column);
  hit.textNodeId = cell->id();
  hit.tableRow = row;
  hit.tableColumn = column;
  return hit;
}

void testInsertChecklistColumnAfterFillsHeaderAndGlyphs() {
  DocumentSession session;
  EditorController controller;
  controller.attach(&session, nullptr);

  session.setMarkdownText(QStringLiteral("| A | B |\n| --- | --- |\n| 1 | 2 |"), false);
  MarkdownNode* table = firstBlockOfType(session, BlockType::Table);
  require(table != nullptr, "table should exist");
  MarkdownNode* cell = childAt(childAt(table, 1), 1);

  HitTestResult cellHit;
  cellHit.zone = HitTestResult::Zone::TableCell;
  cellHit.blockId = table->id();
  cellHit.textNodeId = cell->id();
  cellHit.tableRow = 1;
  cellHit.tableColumn = 1;
  controller.activateHit(cellHit);

  const QString unchecked = checklistGlyphPairs().at(1).unchecked; // ☐
  require(controller.tableController().insertChecklistColumn(1, unchecked),
          "insert checklist column after should succeed");

  MarkdownNode* reparsed = firstBlockOfType(session, BlockType::Table);
  require(reparsed != nullptr, "table should still exist");
  require(TableModelOps::columnCount(*reparsed) == 3, "checklist column should add one column");
  require(cellText(*TableModelOps::cellAt(*reparsed, 0, 2)) == QStringLiteral("#"),
          "checklist header cell should be #");
  require(cellText(*TableModelOps::cellAt(*reparsed, 1, 2)) == unchecked,
          "checklist body cell should hold the hollow glyph");
  require(cellText(*TableModelOps::cellAt(*reparsed, 0, 0)) == QStringLiteral("A"),
          "existing cells should be untouched");
}

void testInsertChecklistColumnBeforePlacesColumnFirst() {
  DocumentSession session;
  EditorController controller;
  controller.attach(&session, nullptr);

  session.setMarkdownText(QStringLiteral("| A | B |\n| --- | --- |\n| 1 | 2 |"), false);
  MarkdownNode* table = firstBlockOfType(session, BlockType::Table);
  MarkdownNode* cell = childAt(childAt(table, 1), 0);
  HitTestResult cellHit;
  cellHit.zone = HitTestResult::Zone::TableCell;
  cellHit.blockId = table->id();
  cellHit.textNodeId = cell->id();
  cellHit.tableRow = 1;
  cellHit.tableColumn = 0;
  controller.activateHit(cellHit);

  const QString unchecked = checklistGlyphPairs().at(0).unchecked; // ☆
  require(controller.tableController().insertChecklistColumn(-1, unchecked),
          "insert checklist column before should succeed");
  MarkdownNode* reparsed = firstBlockOfType(session, BlockType::Table);
  require(cellText(*TableModelOps::cellAt(*reparsed, 0, 0)) == QStringLiteral("#"),
          "checklist column should land first");
  require(cellText(*TableModelOps::cellAt(*reparsed, 1, 0)) == unchecked, "body glyph should land in the first column");
}

void testToggleChecklistCellFlipsBothWays() {
  DocumentSession session;
  EditorController controller;
  controller.attach(&session, nullptr);

  session.setMarkdownText(QStringLiteral("| # | 事项 |\n| --- | --- |\n| ☐ | 写文档 |"), false);
  MarkdownNode* table = firstBlockOfType(session, BlockType::Table);
  MarkdownNode* cell = childAt(childAt(table, 1), 0);
  HitTestResult cellHit;
  cellHit.zone = HitTestResult::Zone::TableCell;
  cellHit.blockId = table->id();
  cellHit.textNodeId = cell->id();
  cellHit.tableRow = 1;
  cellHit.tableColumn = 0;
  controller.activateHit(cellHit);

  require(controller.tableController().toggleChecklistCell(), "toggle hollow → checked should succeed");
  MarkdownNode* reparsed = firstBlockOfType(session, BlockType::Table);
  require(cellText(*TableModelOps::cellAt(*reparsed, 1, 0)) == QStringLiteral("☑️"),
          "hollow glyph should become the checked glyph");

  controller.activateHit(cellHitWith(*reparsed, 1, 0));
  require(controller.tableController().toggleChecklistCell(), "toggle checked → hollow should succeed");
  reparsed = firstBlockOfType(session, BlockType::Table);
  require(cellText(*TableModelOps::cellAt(*reparsed, 1, 0)) == QStringLiteral("☐"),
          "checked glyph should become hollow again");
}

void testToggleIgnoresPlainTextCell() {
  DocumentSession session;
  EditorController controller;
  controller.attach(&session, nullptr);

  session.setMarkdownText(QStringLiteral("| # | 事项 |\n| --- | --- |\n| ☐ | 写文档 |"), false);
  MarkdownNode* table = firstBlockOfType(session, BlockType::Table);
  controller.activateHit(cellHitWith(*table, 1, 1));

  const QString before = session.markdownText().toString();
  require(!controller.tableController().toggleChecklistCell(), "plain text cell should not toggle");
  require(session.markdownText().toString() == before, "plain text cell should stay untouched");
}

void testToggleChecksStarPairVariant() {
  DocumentSession session;
  EditorController controller;
  controller.attach(&session, nullptr);

  // ☑ without the emoji-variation selector must still match the ☑️/☐ pair.
  session.setMarkdownText(QStringLiteral("| # | 事项 |\n| --- | --- |\n| ☑ | 写文档 |"), false);
  MarkdownNode* table = firstBlockOfType(session, BlockType::Table);
  controller.activateHit(cellHitWith(*table, 1, 0));
  require(controller.tableController().toggleChecklistCell(), "bare ☑ should toggle");
  MarkdownNode* reparsed = firstBlockOfType(session, BlockType::Table);
  require(cellText(*TableModelOps::cellAt(*reparsed, 1, 0)) == QStringLiteral("☐"),
          "bare ☑ should switch to the hollow glyph");
}

} // namespace

int main(int argc, char** argv) {
  if (qgetenv("QT_QPA_PLATFORM").isEmpty()) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
  }
  QApplication app(argc, argv);
#define RUN_TEST(test) runTest(#test, test)
  RUN_TEST(testInsertChecklistColumnAfterFillsHeaderAndGlyphs);
  RUN_TEST(testInsertChecklistColumnBeforePlacesColumnFirst);
  RUN_TEST(testToggleChecklistCellFlipsBothWays);
  RUN_TEST(testToggleIgnoresPlainTextCell);
  RUN_TEST(testToggleChecksStarPairVariant);
#undef RUN_TEST
  return 0;
}