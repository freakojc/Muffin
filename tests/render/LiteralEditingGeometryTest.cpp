#include "EditorViewTestUtils.h"

#include "blocks/code/CodeFenceScrollController.h"
#include "render/TextLayout.h"
#include "theme/CssThemeMapper.h"

#include <QFile>
#include <QImage>
#include <QInputMethodEvent>
#include <QPainter>
#include <QScrollBar>

namespace {

MarkdownNode* findLiteral(MarkdownNode& node, BlockType type) {
  if (node.type() == type) return &node;
  for (const auto& child : node.children())
    if (auto* found = findLiteral(*child, type)) return found;
  return nullptr;
}

HitTestResult::Zone sourceZone(BlockType type) {
  switch (type) {
    case BlockType::MathBlock: return HitTestResult::Zone::Math;
    case BlockType::HtmlBlock: return HitTestResult::Zone::Html;
    case BlockType::FrontMatter: return HitTestResult::Zone::FrontMatter;
    default: return HitTestResult::Zone::Code;
  }
}

struct Fixture {
  DocumentSession session;
  EditorView view;
  EditorController controller;
  NodeId id;

  Fixture(RenderTheme theme, QString markdown, BlockType type, int width = 420, int zoom = 100) {
    controller.attach(&session, &view);
    view.resize(width, 800);
    view.setTheme(std::move(theme));
    view.setZoomPercent(zoom);
    session.setMarkdownText(std::move(markdown), false);
    view.setDocument(session.document());
    auto* node = findLiteral(session.document().root(), type);
    require(node != nullptr, "source fixture should parse to the requested block");
    id = node->id();
    HitTestResult hit;
    hit.zone = sourceZone(type);
    hit.blockId = hit.textNodeId = id;
    controller.activateHit(hit);
    if (type == BlockType::HtmlBlock) {
      require(controller.htmlLiteral().enterEditMode(), "HTML source editing should activate");
      view.setEditingHtmlBlock(id);
    }
  }

  const BlockLayout& block() { return *requireViewBlock(view, id, QStringLiteral("source geometry")); }
  MarkdownNode& node() {
    auto* result = session.document().node(id);
    require(result != nullptr, "edited source node must remain addressable");
    return *result;
  }
  void cursor(qsizetype offset) {
    CursorPosition position;
    position.blockId = position.text.nodeId = id;
    position.text.textOffset = offset;
    controller.selection().setCursorPosition(position);
  }
};

void requireNear(qreal actual, qreal expected, const QString& context) {
  require(qAbs(actual - expected) < .02, context + QStringLiteral(": actual=%1 expected=%2").arg(actual).arg(expected));
}

void verifyRoundTrip(Fixture& fixture) {
  const auto& theme = fixture.view.theme();
  const QString source = fixture.node().literal();
  const QString markdownBefore = fixture.session.markdownText().toString();
  require(fixture.block().literal() == source, "display source must retain the exact edit buffer");
  require(fixture.view.inputMethodQuery(Qt::ImFont).value<QFont>() == fixture.block().literalFont(),
          "IME font must be the source block's resolved font");
  for (qsizetype offset = 0; offset <= source.size(); ++offset) {
    // A UTF-16 surrogate pair and a combining sequence have one visual caret.
    if (offset < source.size() && (source[offset].isLowSurrogate() || source[offset].isMark())) continue;
    CursorPosition position;
    position.blockId = position.text.nodeId = fixture.id;
    position.text.textOffset = offset;
    const auto caret = fixture.view.hitForCursorPosition(position);
    const auto clicked = fixture.view.hitTest(fixture.view.mapDocumentToViewport(caret.cursorRect.center()));
    require(clicked.blockId == fixture.id && clicked.textOffset == offset,
            QStringLiteral("caret/click offset mismatch: type=%1 zoom=%2 offset=%3 hit=%4 caret=(%5,%6) content=(%7,%8,%9) direct=%10")
                .arg(int(fixture.node().type())).arg(theme.zoomPercent()).arg(offset).arg(clicked.textOffset)
                .arg(caret.cursorRect.x()).arg(caret.cursorRect.y())
                .arg(fixture.block().literalContentRect(theme).x()).arg(fixture.block().literalContentRect(theme).y())
                .arg(fixture.block().literalContentRect(theme).width())
                .arg(fixture.block().literalTextLayout(theme).offsetAtPoint(caret.cursorRect.center() - fixture.block().literalContentRect(theme).topLeft())));
    requireNear(clicked.cursorRect.x(), caret.cursorRect.x(), "click and programmatic caret x");
    requireNear(clicked.cursorRect.y(), caret.cursorRect.y(), "click and programmatic caret y");
    const int line = fixture.block().literalVisualLineIndexForOffset(offset, theme);
    require(fixture.block().literalOffsetAtVisualLineX(line,
                caret.cursorRect.left() - fixture.block().literalContentRect(theme).left(), theme) == offset,
            "vertical navigation must use the painted source line and column");
  }

  qsizetype offset = qMin<qsizetype>(7, source.size());
  while (offset < source.size() && (source[offset].isLowSurrogate() || source[offset].isMark())) ++offset;
  fixture.cursor(offset);
  const auto clicked = fixture.view.hitTest(fixture.view.mapDocumentToViewport(fixture.view.effectiveCursorRect().center()));
  fixture.controller.activateHit(clicked);
  const QRectF before = fixture.view.hitForCursorPosition(fixture.controller.selection().cursorPosition()).cursorRect;
  requireNear(fixture.view.effectiveCursorRect().x(), before.x(), "activated click caret x");
  require(fixture.controller.inputController().insertText(QStringLiteral("Z")), "typing at a clicked source caret should work");
  QString expected = source;
  expected.insert(offset, QLatin1Char('Z'));
  fixture.id = fixture.controller.selection().cursorPosition().blockId;
  require(fixture.node().literal() == expected,
          QStringLiteral("typing must edit the visible caret: type=%1 clicked=%2 expected=[%3] actual=[%4]")
              .arg(int(fixture.node().type())).arg(clicked.textOffset).arg(expected, fixture.node().literal()));
  require(fixture.controller.inputController().deleteBackward(), "source backspace should work");
  fixture.id = fixture.controller.selection().cursorPosition().blockId;
  require(fixture.node().literal() == source, "backspace should remove precisely the character just inserted");
  require(fixture.session.markdownText().toString() == markdownBefore,
          QStringLiteral("typing/backspace must preserve surrounding Markdown: type=%1 before=[%2] after=[%3]")
              .arg(int(fixture.node().type())).arg(markdownBefore, fixture.session.markdownText().toString()));
}

void testRealThemesAndZoom() {
  const QString line = QStringLiteral("abcdefghijklmnopqrstuvwxyz").repeated(3);
  for (const QString id : {QStringLiteral("github"), QStringLiteral("newsprint"), QStringLiteral("night"),
                          QStringLiteral("pixyll"), QStringLiteral("whitey")}) {
    require(QFile::exists(QStringLiteral(":/themes/%1.css").arg(id)), "real theme resources must be embedded in this test");
    const auto definition = ThemeDefinition::builtIn(id);
    require(definition.has_value(), "built-in theme must load instead of silently falling back");
    for (int zoom : {100, 125, 150}) {
      for (int width : {420, 780}) {
        Fixture math(RenderTheme::fromDefinition(*definition), QStringLiteral("$$\n%1\nxyz=12345\n$$").arg(line),
                     BlockType::MathBlock, width, zoom);
        require(math.block().literalFont() == math.view.theme().codeFont(),
                "synthetic math source pre must carry the same host classes as code source");
        verifyRoundTrip(math);
        Fixture code(RenderTheme::fromDefinition(*definition), QStringLiteral("```\n%1\nxyz=12345\n```").arg(line),
                     BlockType::CodeFence, width, zoom);
        verifyRoundTrip(code);
      }
    }
  }
}

void testScopedCssAndOtherSourceBlocks() {
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "pre{font-family:Consolas;font-size:12px;line-height:1.5}h1 + pre{font-size:24px;line-height:2}", "source-local", {}));
  Fixture scoped(theme, QStringLiteral("# Heading\n\n```\nabcdefghijklmnop\n1234567890\n```"), BlockType::CodeFence);
  require(scoped.block().literalFont() != scoped.view.theme().codeFont(), "fixture must exercise live-node CSS rather than global style");
  verifyRoundTrip(scoped);
  Fixture html(RenderTheme::pixyll(), QStringLiteral("  <div>\n  abcdefghijklmnop\n  </div>\n"), BlockType::HtmlBlock);
  verifyRoundTrip(html);
  Fixture frontMatter(RenderTheme::github(), QStringLiteral("---\ntitle: abcdefghijklmnop\nkey: value\n---\n"), BlockType::FrontMatter);
  verifyRoundTrip(frontMatter);
  Fixture invalidMath(RenderTheme::pixyll(), QStringLiteral("$$\n\\unknowncommand{{\nabcdefghijklmnop\n$$"), BlockType::MathBlock);
  verifyRoundTrip(invalidMath);
  Fixture unicode(RenderTheme::github(), QString::fromUtf8("```\n中文\tfoo😀ébar\n\nend\n```"), BlockType::CodeFence);
  verifyRoundTrip(unicode);
}

void testSourcePaintUsesTheMeasuredGlyphs() {
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "pre{font-family:Consolas;font-size:19px;line-height:2;background:white;border:0;padding:8px;color:black}", "source-paint", {}));
  Fixture fixture(theme, QStringLiteral("```\nabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\nxyz123\n```"), BlockType::CodeFence);
  const auto& block = fixture.block();
  const auto content = block.literalContentRect(fixture.view.theme());
  QImage actual(900, 800, QImage::Format_ARGB32_Premultiplied), expected(actual.size(), actual.format());
  actual.fill(Qt::white);
  expected.fill(Qt::white);
  {
    QPainter painter(&actual);
    block.paint(painter, fixture.view.theme(), 0);
  }
  // Independent Qt shaping oracle: no BlockLayout or LiteralTextLayout drawing.
  {
    QPainter painter(&expected);
    painter.setPen(Qt::black);
    qreal y = 0;
    for (const auto& text : block.literal().split(QLatin1Char('\n'))) {
      TextLayout layout(text, block.literalFont());
      QTextOption option;
      option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
      layout.setTextOption(option);
      layout.beginLayout();
      for (;;) {
        auto line = layout.createLine();
        if (!line.isValid()) break;
        line.setLineWidth(content.width());
        const qreal height = qMax(block.literalLineHeight(), line.height());
        line.setPosition({0, y + (height - line.height()) * .5});
        y += height;
      }
      layout.endLayout();
      layout.draw(&painter, content.topLeft());
    }
  }
  require(actual.copy(content.toAlignedRect()) == expected.copy(content.toAlignedRect()),
          "painted source glyphs must occupy the measured source layout positions");
}

void testSelectionAndLayoutChanges() {
  SettingsOverride lineNumbers("markdown/showLineNumbers", true);
  const QString source = QStringLiteral("alpha beta gamma delta epsilon zeta eta theta ").repeated(3) +
                         QStringLiteral("\nsecond line with trailing spaces   \n\nend");
  for (const auto type : {BlockType::MathBlock, BlockType::CodeFence}) {
    Fixture fixture(RenderTheme::pixyll(), type == BlockType::MathBlock
        ? QStringLiteral("$$\n%1\n$$").arg(source) : QStringLiteral("```\n%1\n```").arg(source), type);
    const auto& theme = fixture.view.theme();
    const auto first = fixture.block().literalVisualCursorRect(2, theme);
    const auto last = fixture.block().literalVisualCursorRect(7, theme);
    const auto selection = fixture.block().selectionRectsForOffsets(2, 7, theme);
    require(selection.size() == 1, "same-line source selection should occupy one row");
    requireNear(selection.front().left(), first.left() - 1, "selection begins at the shaped caret");
    requireNear(selection.front().right(), last.left() + 1, "selection ends at the shaped caret");
    requireNear(selection.front().top(), first.top(), "selection row top");
    requireNear(selection.front().height(), first.height(), "selection row height");
    const auto multiline = fixture.block().selectionRectsForOffsets(2, source.size() - 2, theme);
    require(multiline.size() > 2, "wrapped source selection should span its visual rows");
    requireNear(multiline.front().top(), first.top(), "multiline selection starts on the caret row");
    const auto end = fixture.block().literalVisualCursorRect(source.size() - 2, theme);
    requireNear(multiline.back().top(), end.top(), "multiline selection ends on the caret row");
    requireNear(multiline.back().right(), end.left() + 1, "multiline selection end column");
    verifyRoundTrip(fixture);
    const auto rowsBefore = fixture.block().literalVisualLineCount(theme);
    fixture.view.setContentWidthPx(900);
    fixture.view.resize(1000, 800);
    fixture.view.setZoomPercent(100); // rebuild at the changed viewport width
    require(fixture.block().literalVisualLineCount(fixture.view.theme()) < rowsBefore,
            "resizing must invalidate wrapping and rebuild source geometry");
    verifyRoundTrip(fixture);
    fixture.view.setZoomPercent(150);
    verifyRoundTrip(fixture);
  }
}

int pixelsNearColor(const QImage& image, QRect area, const QColor& color) {
  int result = 0;
  area = area.intersected(image.rect());
  for (int y = area.top(); y <= area.bottom(); ++y)
    for (int x = area.left(); x <= area.right(); ++x) {
      const auto pixel = image.pixelColor(x, y);
      if (qAbs(pixel.red() - color.red()) + qAbs(pixel.green() - color.green()) + qAbs(pixel.blue() - color.blue()) < 45)
        ++result;
    }
  return result;
}

void testCodeHighlightColorsReachPaintedGlyphs() {
  const QString source = QStringLiteral("#include <QApplication>\n\nint main(int argc, char** argv) {\n"
      "  // 中文 😀\n  QApplication app(argc, argv);\n  return app.exec();\n}\n");
  for (const auto& name : {QStringLiteral("github"), QStringLiteral("night")}) {
    const auto definition = ThemeDefinition::builtIn(name);
    require(definition.has_value(), "code highlight fixture theme exists");
    Fixture fixture(RenderTheme::fromDefinition(*definition), QStringLiteral("```cpp\n%1```\n").arg(source), BlockType::CodeFence, 900);
    const auto& theme = fixture.view.theme();
    const auto& block = fixture.block();
    require(!block.codeHighlightSpans().isEmpty(), "C++ syntax parser supplies highlight spans to the block");
    QImage image(1000, 800, QImage::Format_ARGB32_Premultiplied);
    image.fill(theme.backgroundColor());
    {
      QPainter painter(&image);
      block.paint(painter, theme, 0);
    }
    if (const auto path = qEnvironmentVariable("MUFFIN_CODE_HIGHLIGHT_SCREENSHOT"); !path.isEmpty())
      require(image.save(path + QLatin1Char('.') + name + QStringLiteral(".png")), "capture actual code highlight painting");
    const auto requireTokenColor = [&](const QString& token, CodeHighlightRole role) {
      const auto start = source.indexOf(token);
      const auto color = theme.codeHighlightColor(role);
      const auto caret = block.literalVisualCursorRect(start, theme);
      const auto end = block.literalVisualCursorRect(start + token.size(), theme);
      const QRect area = QRectF(caret.topLeft(), QPointF(end.left(), caret.bottom())).toAlignedRect();
      QImage reference(image.size(), image.format());
      reference.fill(image.pixelColor(int(block.literalContentRect(theme).right()) - 2, int(caret.center().y())));
      {
        QPainter painter(&reference);
        painter.setPen(color);
        block.literalTextLayout(theme).draw(painter, block.literalContentRect(theme).topLeft(), {});
      }
      const int expectedInk = pixelsNearColor(reference, area, color);
      require(expectedInk >= 4 && pixelsNearColor(image, area, color) >= expectedInk * .9,
              QStringLiteral("entire %1 token retains its syntax color in %2").arg(token, name));
    };
    requireTokenColor(QStringLiteral("return"), CodeHighlightRole::Keyword);
    requireTokenColor(QStringLiteral("exec"), CodeHighlightRole::Function);
    requireTokenColor(QStringLiteral("int"), CodeHighlightRole::Type);
    verifyRoundTrip(fixture);
  }
}

QImage sourcePaintOracle(const QString& text, const QFont& font, qreal width, qreal lineHeight,
                         const QList<TextLayout::FormatRange>& formats, const QColor& ink) {
  QImage image(500, 700, QImage::Format_ARGB32_Premultiplied);
  image.fill(Qt::white);
  QPainter painter(&image);
  painter.setPen(ink);
  qreal y = 0;
  int start = 0;
  for (const auto& paragraph : text.split(QLatin1Char('\n'))) {
    TextLayout layout(paragraph, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    option.setFlags(QTextOption::IncludeTrailingSpaces);
    layout.setTextOption(option);
    QList<TextLayout::FormatRange> local;
    for (const auto& range : formats) {
      const int first = qMax(start, range.start), last = qMin(start + int(paragraph.size()), range.start + range.length);
      if (first < last) local.append({first - start, last - first, range.format});
    }
    layout.setFormats(local);
    layout.beginLayout();
    for (;;) {
      auto line = layout.createLine();
      if (!line.isValid()) break;
      line.setLineWidth(width);
      const auto height = qMax(lineHeight, line.height());
      line.setPosition({0, y + (height - line.height()) * .5});
      y += height;
    }
    layout.endLayout();
    layout.draw(&painter, {8, 8});
    start += int(paragraph.size()) + 1;
  }
  return image;
}

void testSourcePaintFormatCascadeAndInvalidation() {
  const QString source = QStringLiteral("alpha beta gamma delta\nreturn app.exec();\nend");
  const int keyword = int(source.indexOf(QStringLiteral("return")));
  QTextCharFormat ime;
  ime.setForeground(QColor("#008000"));
  ime.setFontUnderline(true);
  LiteralTextLayout::Preedit preedit{keyword + 4, QStringLiteral("IME"), {{0, 3, ime}}, 2};
  const QFont font = RenderTheme::github().codeFont();
  LiteralTextLayout layout(source, font, 135, 28, true, preedit);
  QString display = source;
  display.insert(preedit.offset, preedit.text);
  QTextCharFormat diagnostic;
  diagnostic.setBackground(QColor("#ffe4b5"));
  diagnostic.setUnderlineStyle(QTextCharFormat::WaveUnderline);
  diagnostic.setUnderlineColor(Qt::red);
  for (const auto color : {QColor("#9b008b"), QColor("#0000a8")}) {
    QTextCharFormat base, syntax;
    base.setForeground(Qt::black);
    syntax.setForeground(color);
    const QList<TextLayout::FormatRange> formats{{0, int(source.size()), base}, {keyword, 6, syntax},
        {keyword + 2, 2, base}, {keyword + 2, 13, diagnostic}};
    // Independent expected display ranges, including the hole occupied by IME.
    const QList<TextLayout::FormatRange> expectedFormats{
        {0, int(display.size()), base}, {keyword, 4, syntax}, {keyword + 7, 2, syntax}, {keyword + 2, 2, base},
        {keyword + 2, 2, diagnostic}, {keyword + 7, 11, diagnostic}, {keyword + 4, 3, ime}};
    const auto expected = sourcePaintOracle(display, font, 135, 28, expectedFormats, Qt::black);
    for (int repeat = 0; repeat < 2; ++repeat) {
      QImage actual(expected.size(), expected.format());
      actual.fill(Qt::white);
      { QPainter painter(&actual); painter.setPen(Qt::black); layout.draw(painter, {8, 8}, formats); }
      require(actual == expected, "cached source formats merge syntax, diagnostics and IME across physical lines and wrapping");
    }
  }
  QImage plain(500, 700, QImage::Format_ARGB32_Premultiplied);
  plain.fill(Qt::white);
  { QPainter painter(&plain); painter.setPen(Qt::black); layout.draw(painter, {8, 8}, {}); }
  require(plain == sourcePaintOracle(display, font, 135, 28, {{keyword + 4, 3, ime}}, Qt::black),
          "removing source styles discards cached colors and diagnostics while retaining IME");
}

void testOverflowingTrailingSpaceCarets() {
  // Qt keeps a breakable whitespace suffix on the preceding wrapped row.
  // Its editable caret advances can extend beyond the source block's box,
  // particularly with the wider monospace fallback used on Linux.
  const QString firstLine = QStringLiteral("second line with trailing spaces   ");
  const QString source = firstLine + QStringLiteral("\nnext row\nend");
  for (const auto type : {BlockType::MathBlock, BlockType::CodeFence}) {
    auto themeDefinition = CssThemeMapper::fromCss(
        "pre{font-family:monospace;font-size:20px;line-height:1.5;padding:0}", "source-overflow", {});
    bool exercisedOverflow = false;
    for (int width = 300; width <= 460 && !exercisedOverflow; ++width) {
      Fixture fixture(RenderTheme::fromDefinition(themeDefinition), type == BlockType::MathBlock
          ? QStringLiteral("$$\n%1\n$$").arg(source) : QStringLiteral("```\n%1\n```").arg(source), type, width);
      const auto& theme = fixture.view.theme();
      const auto caret = fixture.block().literalVisualCursorRect(firstLine.size(), theme);
      if (caret.left() <= fixture.block().rect().right() + 2) continue;
      exercisedOverflow = true;
      verifyRoundTrip(fixture);
    }
    require(exercisedOverflow, "fixture must exercise a source caret outside the block hit box");
  }
}

void testHorizontalScrollAndNavigation() {
  SettingsOverride wrapOff("markdown/codeBlockWrap", false);
  Fixture fixture(RenderTheme::github(), QStringLiteral("```\n%1\n%1\n```").arg(QString(180, QLatin1Char('a'))), BlockType::CodeFence);
  fixture.cursor(100);
  const auto& theme = fixture.view.theme();
  const auto& block = fixture.block();
  const qreal offset = block.literalVisualCursorRect(100, theme).x() - block.literalContentRect(theme).left() - 40;
  fixture.controller.codeFenceScroll().setOffset(fixture.id, offset);
  const QRectF visible = fixture.view.effectiveCursorRect();
  const auto clicked = fixture.view.hitTest(fixture.view.mapDocumentToViewport(visible.center()));
  require(clicked.textOffset == 100, "scrolled visible caret must hit the same edit offset");
  requireNear(visible.x(), block.literalVisualCursorRect(100, theme).x() - offset, "horizontal caret transform");
  require(pressKey(fixture.controller.inputController(), &fixture.view, Qt::Key_Down), "Down inside source should work");
  require(fixture.controller.selection().cursorPosition().text.textOffset == 281, "Down should preserve the actual code column");
  require(pressKey(fixture.controller.inputController(), &fixture.view, Qt::Key_Up), "Up inside source should work");
  require(fixture.controller.selection().cursorPosition().text.textOffset == 100, "Up should return to the original column");

  const QString committed = fixture.node().literal();
  QInputMethodEvent composition(QString(50, QLatin1Char('M')), {{QInputMethodEvent::Cursor, 50, 1, {}}});
  QApplication::sendEvent(&fixture.view, &composition);
  const QRectF composed = fixture.block().literalPreeditCursorRect(theme);
  const QRectF composedVisible = fixture.view.effectiveCursorRect();
  const QRectF content = fixture.block().literalContentRect(theme);
  require(composedVisible.left() >= content.left() && composedVisible.left() <= content.right(),
          "horizontal scrolling must keep the IME composition caret visible");
  requireNear(composedVisible.x(), composed.x() - fixture.controller.codeFenceScroll().offsetFor(fixture.id),
              "scrolled composition caret must use the source layout geometry");
  require(fixture.node().literal() == committed, "scrolling composition must preserve committed text");
  QInputMethodEvent cancel;
  QApplication::sendEvent(&fixture.view, &cancel);

  Fixture math(RenderTheme::pixyll(), QStringLiteral("$$\nabcdefghijk\nabcdefghijk\n$$"), BlockType::MathBlock);
  math.cursor(5);
  require(pressKey(math.controller.inputController(), &math.view, Qt::Key_Down), "math source Down should work");
  require(math.controller.selection().cursorPosition().text.textOffset == 17,
          "math source navigation must preserve the rendered column on identical rows");
}

void testPreeditReflowsSourceAndCommitsAtCaret() {
  for (const auto type : {BlockType::MathBlock, BlockType::CodeFence, BlockType::HtmlBlock, BlockType::FrontMatter}) {
    const QString markdown = type == BlockType::MathBlock ? QStringLiteral("$$\nabcdefghijk\n$$") :
        type == BlockType::HtmlBlock ? QStringLiteral("<div>abcdefghijk</div>\n") :
        type == BlockType::FrontMatter ? QStringLiteral("---\ntitle: abcdefghijk\n---") :
        QStringLiteral("```\nabcdefghijk\n```");
    Fixture fixture(RenderTheme::pixyll(), markdown, type, 420, 125);
    const qsizetype offset = type == BlockType::HtmlBlock ? 6 : 3;
    fixture.cursor(offset);
    const QString before = fixture.session.markdownText().toString();
    const QString source = fixture.node().literal();
    const QRectF suffixBefore = fixture.block().literalVisualCursorRect(source.size(), fixture.view.theme());
    const QString composition = QStringLiteral("MMMM").repeated(20);
    QInputMethodEvent preedit(composition, {{QInputMethodEvent::Cursor, 3, 1, {}}});
    QApplication::sendEvent(&fixture.view, &preedit);
    require(fixture.session.markdownText().toString() == before, "preedit must not mutate committed Markdown");
    require(fixture.block().literalTextLayout(fixture.view.theme()).hasPreedit(), "source must own the display splice");
    const QRectF suffixAfter = fixture.block().literalVisualCursorRect(source.size(), fixture.view.theme());
    require(suffixAfter.top() > suffixBefore.top(), "long preedit must wrap and push following source down");
    const auto compositionCaret = fixture.block().literalPreeditCursorRect(fixture.view.theme());
    requireNear(fixture.view.effectiveCursorRect().x(), compositionCaret.x(), "visible composition caret x");
    requireNear(fixture.view.effectiveCursorRect().y(), compositionCaret.y(), "visible composition caret y");
    require(fixture.view.inputMethodQuery(Qt::ImCursorRectangle).toRect() ==
                fixture.view.effectiveCursorRect().translated(0, -fixture.view.verticalScrollBar()->value()).toRect(),
            "IME candidate rectangle must follow the composition caret");

    // IMEs move their caret/change formatting without changing the text itself.
    QInputMethodEvent move(composition, {{QInputMethodEvent::Cursor, 8, 1, {}}});
    QApplication::sendEvent(&fixture.view, &move);
    const auto moved = fixture.block().literalPreeditCursorRect(fixture.view.theme());
    require(moved != compositionCaret, "same-text preedit cursor changes must refresh geometry");
    QTextCharFormat enlarged;
    enlarged.setFontPointSize(48);
    QInputMethodEvent reformat(composition, {{QInputMethodEvent::Cursor, 8, 1, {}},
        {QInputMethodEvent::TextFormat, 0, int(composition.size()), enlarged}});
    QApplication::sendEvent(&fixture.view, &reformat);
    require(fixture.block().literalPreeditCursorRect(fixture.view.theme()).height() > moved.height(),
            "same-text IME format changes must reshape source and caret together");
    QInputMethodEvent cancel;
    QApplication::sendEvent(&fixture.view, &cancel);
    require(!fixture.block().literalTextLayout(fixture.view.theme()).hasPreedit(), "cancel must remove the display splice");
    require(fixture.session.markdownText().toString() == before, "cancel must preserve committed text");
    QInputMethodEvent commit(QStringLiteral("next"), {{QInputMethodEvent::Cursor, 2, 1, {}}});
    commit.setCommitString(QStringLiteral("XYZ"));
    QApplication::sendEvent(&fixture.view, &commit);
    fixture.id = fixture.controller.selection().cursorPosition().blockId;
    QString expected = source;
    expected.insert(offset, QStringLiteral("XYZ"));
    require(fixture.node().literal() == expected, "IME commit must edit the original logical caret position");
    const auto& continued = fixture.block().literalTextLayout(fixture.view.theme());
    require(continued.hasPreedit() && continued.preedit().offset == offset + 3,
            "commit and new composition in one event must splice at the advanced logical caret");
    QApplication::sendEvent(&fixture.view, &cancel);
    require(fixture.node().literal() == expected, "canceling the next composition must preserve the committed text");
  }
}

}  // namespace

int main(int argc, char** argv) {
  qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QCoreApplication::setOrganizationName("MuffinTest");
  QCoreApplication::setApplicationName("LiteralEditingGeometryTest");
  SettingsOverride wrap("markdown/codeBlockWrap", true);
  SettingsOverride lineNumbers("markdown/showLineNumbers", false);
  runTest("painted code syntax colors", testCodeHighlightColorsReachPaintedGlyphs);
  runTest("source format cascade and invalidation", testSourcePaintFormatCascadeAndInvalidation);
  runTest("real themes, zoom and wrap", testRealThemesAndZoom);
  runTest("local CSS, HTML, metadata and Unicode", testScopedCssAndOtherSourceBlocks);
  runTest("painted glyph geometry", testSourcePaintUsesTheMeasuredGlyphs);
  runTest("selection, whitespace and layout changes", testSelectionAndLayoutChanges);
  runTest("overflowing trailing space carets", testOverflowingTrailingSpaceCarets);
  runTest("horizontal scroll and vertical navigation", testHorizontalScrollAndNavigation);
  runTest("source preedit and commit", testPreeditReflowsSourceAndCommitsAtCaret);
  QApplication::clipboard()->clear();
}
