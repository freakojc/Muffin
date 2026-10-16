#include "theme/CssContent.h"
#include "theme/CssComputedStyleEngine.h"
#include "theme/CssThemeMapper.h"
#include "theme/DocumentStyleTree.h"
#include "theme/NodeCssElement.h"
#include "document/DocumentSession.h"
#include "render/CssPaintAnimation.h"
#include "render/DocumentLayout.h"
#include "render/LayoutBox.h"

#include <QApplication>
#include <QBuffer>
#include <QPainter>
#include "../render/RenderTestUtils.h"

using namespace muffin;

namespace {
ThemeElementStyle project(const QString& declarations) {
  DocumentStylePrototypes tree;
  CssComputedStyleEngine engine(CssThemeParser::parse("#write{" + declarations + "}", {}));
  return CssThemeMapper::projectComputedStyle("#write", engine.styleFor(tree.element("#write")));
}

QImage raster(const ThemeElementStyle& style, ThemeElementBoxStyle used = {}) {
  QFont font; font.setPointSizeF(style.text.fontSizePx * .75);
  QImage image(100, 100, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::transparent);
  QPainter painter(&image);
  paintLayoutBox(painter, LayoutBox::place("#write", style, used, {0, 0, 100, 100}, font));
  return image;
}

void testLayeredBackgrounds() {
  const auto initial = project("background-position:20% 30%");
  require(initial.paint.backgroundLayers.size() == 1 && qAbs(initial.paint.backgroundLayers.front().positionX.fraction-.2)<.001,
          "implicit background-image none retains the initial layer and its animation inputs");
  auto style = project("background:linear-gradient(rgba(255,0,0,.5),rgba(255,0,0,.5)),linear-gradient(blue,blue) green");
  require(style.paint.backgroundLayers.size() == 2, "shorthand retains both image layers");
  const auto color = raster(style).pixelColor(50, 50);
  require(qAbs(color.red() - 128) <= 1 && color.green() == 0 && qAbs(color.blue() - 127) <= 1,
          "first CSS image is painted above later images and the color");

  style = project("background-color:green;background-image:none,linear-gradient(red,red),linear-gradient(blue,blue);"
                  "background-size:20px 20px,40px 40px;background-position:0 0,100% 100%;background-repeat:no-repeat");
  require(style.paint.backgroundLayers.size() == 3 && style.paint.backgroundLayers[0].image.kind == GradientSpec::Kind::None,
          "none retains its list slot");
  auto image = raster(style);
  require(image.pixelColor(10,10) == QColor("blue") && image.pixelColor(70,70) == QColor("red") &&
          image.pixelColor(50,50) == QColor("green"), "independent property lists cycle including none layers");
  style = project("background-color:lime;background-image:none,none;background-clip:border-box,content-box");
  ThemeElementBoxStyle used; used.padding = {20,20,20,20};
  image = raster(style, used);
  require(image.pixelColor(10,10).alpha() == 0 && image.pixelColor(50,50) == QColor("lime"),
          "bottom layer clip determines the solid color painting area");

  QImage tile(2,1,QImage::Format_ARGB32); tile.setPixelColor(0,0,Qt::red); tile.setPixelColor(1,0,Qt::blue);
  QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); tile.save(&buffer,"PNG");
  const auto url = "url(data:image/png;base64," + QString::fromLatin1(bytes.toBase64()) + ")";
  style = project("background:green " + url + " center/contain no-repeat");
  image = raster(style);
  require(image.pixelColor(25,10) == QColor("green") && image.pixelColor(25,50) == QColor("red") &&
          image.pixelColor(75,50) == QColor("blue"), "contain preserves aspect ratio and centered free space");
  style = project("background:green " + url + " center/cover no-repeat");
  image = raster(style);
  require(image.pixelColor(25,10) == QColor("red") && image.pixelColor(75,90) == QColor("blue"),
          "cover preserves aspect ratio and clips the centered image");
  style = project("background:green " + url + " right bottom/40px auto no-repeat");
  image = raster(style);
  require(image.pixelColor(65,85) == QColor("red") && image.pixelColor(85,85) == QColor("blue") &&
          image.pixelColor(65,75) == QColor("green"), "auto axis uses the bitmap's intrinsic ratio");

  const auto from = project("background-image:linear-gradient(red,red),linear-gradient(blue,blue);background-position:0% 0%,20% 30%");
  const auto to = project("background-image:linear-gradient(blue,blue),linear-gradient(red,red);background-position:100% 0%,80% 30%");
  auto animated = to;
  interpolateCssPaintProperty(animated,from,to,"background-position-x",.5);
  require(qAbs(animated.paint.backgroundLayers[0].positionX.fraction-.5)<.001 &&
          qAbs(animated.paint.backgroundLayers[1].positionX.fraction-.5)<.001, "animation samples every background layer");
  require(CssThemeSheet::supportsMatches("(background-image: none, linear-gradient(red,blue))"),
          "supports advertises implemented multilayer images");
}

void testAttributeContent() {
  const auto value = [](const QString&) { return 3; };
  const auto chain = [](const QString&) { return QVector<int>{2,3}; };
  const auto attribute = [](const QString& name) -> std::optional<QString> {
    if (name == "data-label") return "A) B";
    if (name == "empty") return QString();
    return {};
  };
  const auto text = resolveContentTokens(parseContentTokens(R"css("["attr(data-label)"]"counter(n) ":" counters(n, ".") attr(missing, " fallback)") attr(empty,"wrong"))css"),value,chain,attribute);
  require(text == "[A) B]3:2.3 fallback)", "attrs, adjacent strings, quoted parentheses and counters compose");
  require(resolveContentTokens(parseContentTokens(R"css("\43 PP" "\"ok\"" / "alternative")css"),value,chain) == "CPP\"ok\"",
          "CSS escapes decode once and alternative text is not painted");
  require(parseContentTokens("attr(unclosed").empty(), "unbalanced attribute functions are rejected");
  require(parseContentTokens("attr(data-value px)").empty(), "unsupported typed attributes are not displayed literally");
  CssElement element; element.tag = "a";
  CssComputedStyleEngine fallback(CssThemeParser::parse("a::before{content:'fallback';content:attr(data-value px)}",{}));
  element.pseudoElement = "before";
  require(fallback.styleFor(element).resolvedValue("content") == "'fallback'", "unsupported attribute types retain the prior declaration");

  DocumentSession session;
  session.setMarkdownText("# Header\n\n```cpp\nint value;\n```\n\n[link](https://example.com)\n",false);
  auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromSheet(CssThemeParser::parse(
      "pre::before{content:'Language: 'attr(lang);position:absolute}a::after{content:' <'attr(href)'>'}",{}),"attr-test"));
  const auto* fence = findFirstBlock(session.document().root(),BlockType::CodeFence);
  const auto* paragraph = findFirstBlock(session.document().root(),BlockType::Paragraph);
  require(fence && paragraph, "attribute fixture parses");
  const auto pseudo = theme.pseudoForNode(*fence,"before");
  require(pseudo && pseudo->content == "Language: cpp", "generated code label reads actual lang attribute");
  const auto quoted = RenderTheme::fromDefinition(CssThemeMapper::fromSheet(CssThemeParser::parse("pre::before{content:'none'}",{}),"quoted"));
  require(quoted.pseudoForNode(*fence,"before")->content == "none", "quoted none remains literal generated text");
  const auto* link = theme.cssInlineElement(*paragraph,paragraph->inlines().front().sourceStart()+1,"a");
  require(link && theme.pseudoForElement(*link,"after")->content == " <https://example.com>", "link content reads the originating href");
  CssElement temporary; temporary.tag="a"; temporary.attributes.insert("href","first");
  require(theme.pseudoForElement(temporary,"after")->content == " <first>", "temporary origins resolve attributes");
  temporary.attributes.insert("href","second");
  require(theme.pseudoForElement(temporary,"after")->content == " <second>", "temporary attribute updates never reuse stale values");
  session.setMarkdownText("```python\nvalue = 1\n```\n",false);
  theme.invalidateDocumentStyles();
  const auto* updated = findFirstBlock(session.document().root(),BlockType::CodeFence);
  require(theme.pseudoForNode(*updated,"before")->content == "Language: python", "document style invalidation refreshes cached attributes");
}

void testTableAndAlertStructure() {
  DocumentSession session;
  const QString source = "Before\n\n|Name|Value|\n|---|---|\n|A|B|\n\nAfter\n\n> [!NOTE]\n> A **semantic** alert.\n";
  session.setMarkdownText(source,false);
  const auto sheet = CssThemeParser::parse("#write>figure.table-figure{padding:12px;border:2px solid red;background:blue}"
      "figure.table-figure>table{border:1px solid green} figure+p{color:#123456}"
      ".md-alert-note{background:linear-gradient(#123456,#123456);border:2px solid #abcdef}"
      ".md-alert-text-note{color:#ff0000;font-weight:700}",{});
  auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromSheet(sheet,"structure-test"));
  NodeCssElementBuilder builder;
  const auto* table = findFirstBlock(session.document().root(),BlockType::Table);
  const auto* quote = findFirstBlock(session.document().root(),BlockType::BlockQuote);
  require(table && quote,"structural fixture parses");
  const auto* tableElement = builder.build(*table);
  require(tableElement->parent->tag=="figure" && tableElement->parent->classes.contains("table-figure"),"table has a real compatibility container");
  CssComputedStyleEngine engine(sheet);
  require(engine.styleFor(*builder.build(*table->nextSibling())).resolvedValue("color")=="#123456","sibling selectors navigate the figure wrapper");
  CssComputedStyleEngine tableQueries(CssThemeParser::parse("table:has(td){color:red}table:has(>thead){padding:7px}",{}));
  require(tableQueries.styleFor(*tableElement).resolvedValue("color") == "red" &&
          tableQueries.styleFor(*tableElement).length("padding-left").px == 7,"table has queries navigate through its semantic sections");
  const auto* title = builder.buildInline(*quote->children().front(),quote->children().front()->inlines().front().sourceStart());
  require(title->classes.contains("md-alert-text-note") && title->parent->classes.contains("md-alert-text-container"),
          "alert title is separately addressable by generic CSS");
  DocumentLayout layout;
  layout.rebuild(session.document(),theme,600);
  const auto* block = layout.block(table->id());
  require(block && block->outerCssBox() && block->cssBorderBox().left()-block->rect().left() >= 14,
          "figure padding and borders participate in table geometry");
  const auto* body = layout.block(quote->children().front()->id());
  require(body && body->inlineLayout() && body->inlineLayout()->visibleText().startsWith("NOTE\nA ") &&
          !body->inlineLayout()->displayText().contains("> "),"alert title and continuation prefixes project without modifying source");
  const auto pos = body->inlineLayout()->visibleText().indexOf("alert");
  const auto caret = body->inlineLayout()->cursorRect(pos);
  const auto hit = body->inlineLayout()->hitTestSourceOffset(caret.center());
  require(qAbs(body->contentSourceStart()+hit-source.indexOf("alert.")) <= 1,"body hit testing retains original source coordinates");
  require(session.document().markdownText().mid(0,source.size()) == source,"semantic rendering preserves the original Markdown");
  const auto* paragraph = quote->children().front().get();
  SelectionRange selection;
  selection.focus.blockId = paragraph->id(); selection.focus.text.nodeId = paragraph->id();
  selection.focus.text.sourceOffset = source.indexOf("[!NOTE]")+2;
  selection.focus.text.textOffset = 2; selection.anchor = selection.focus;
  layout.rebuild(session.document(),theme,600,selection);
  require(layout.block(paragraph->id())->inlineLayout()->visibleText().startsWith("[!NOTE]\nA "),
          "entering an alert title reveals its original editable marker");
  selection.focus.text.sourceOffset = source.indexOf("alert."); selection.focus.text.textOffset = 15; selection.anchor = selection.focus;
  layout.rebuild(session.document(),theme,600,selection);
  require(layout.block(paragraph->id())->inlineLayout()->visibleText().startsWith("NOTE\nA "),
          "leaving the title restores its semantic projection");
}
}  // namespace

int main(int argc,char** argv) {
  QApplication app(argc,argv);
  testLayeredBackgrounds();
  testAttributeContent();
  testTableAndAlertStructure();
  return 0;
}
