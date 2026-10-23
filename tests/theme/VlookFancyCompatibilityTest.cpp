#include "theme/CssComputedStyleEngine.h"
#include "theme/CssThemeMapper.h"
#include "theme/DocumentStyleTree.h"
#include "theme/NodeCssElement.h"
#include "document/DocumentSession.h"
#include "render/DocumentLayout.h"
#include "render/InlineLayout.h"
#include "render/LayoutBox.h"

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QPainter>

#include "../render/RenderTestUtils.h"

using namespace muffin;

namespace {
const MarkdownNode* firstInlineParagraph(const MarkdownNode& root) {
  for (const auto& child : root.children())
    if (child->type() == BlockType::Paragraph && child->inlines().size() > 3)
      return child.get();
  return nullptr;
}

void testSemanticContract(const MarkdownDocument& document) {
  NodeCssElementBuilder builder;
  const auto& root = document.root();
  const auto* write = builder.build(root);
  require(write->parent && write->parent->tag == "content" && write->parent->parent->tag == "body",
          QStringLiteral("live Markdown document uses html/body/content/#write"));
  DocumentStylePrototypes prototypes;
  require(prototypes.element("#write").parent->tag == "content",
          QStringLiteral("prototypes share the live host topology"));
  const auto* list = findFirstBlock(root, BlockType::List);
  const auto* task = findFirstBlock(root, BlockType::ListItem);
  require(list && task && task->isTaskItem() && task->taskChecked(), QStringLiteral("sample has a checked task"));
  require(builder.build(*list)->classes.contains("task-list"), QStringLiteral("task-list class"));
  require(builder.build(*task)->classes.contains("md-task-list-item"), QStringLiteral("md-task-list-item class"));
  const auto* input = builder.build(*task, "input");
  require(input->attributes.value("type") == "checkbox" && input->attributes.contains("checked"),
          QStringLiteral("checkbox attributes"));
  const auto* alert = findFirstBlock(root, BlockType::BlockQuote);
  require(alert && builder.build(*alert)->classes.contains("md-alert-note"),
          QStringLiteral("alert class comes from parsed Markdown"));
  const auto* fence = findFirstBlock(root, BlockType::CodeFence);
  require(fence && builder.build(*fence)->classes.contains("md-fences") &&
              builder.build(*fence)->attributes.value("data-language") == "cpp",
          QStringLiteral("fence language and Typora class"));
  require(builder.build(*fence, "code")->classes.contains("md-fencescode"), QStringLiteral("fence code class"));
  const auto* table = findFirstBlock(root, BlockType::Table);
  require(table && builder.build(*table)->classes.contains("md-table"), QStringLiteral("table semantic class"));
  require(builder.build(*table)->parent->tag == "figure" &&
              builder.build(*table)->parent->classes.contains("table-figure"),
          QStringLiteral("table container is shared with Typora theme selectors"));

  const auto* p = firstInlineParagraph(root);
  require(p, QStringLiteral("sample includes a mixed inline paragraph"));
  const auto contract =
      CssThemeParser::parse(QStringLiteral("content>#write p>span[md-inline=strong]>strong{color:#123456}"
                                           "content>#write p>span[md-inline=em]>em{color:#234567}"
                                           "content>#write p>span[md-inline=code]>code{color:#345678}"
                                           "content>#write p>span[md-inline=strike]>del{color:#456789}"
                                           "ul:has(>.md-task-list-item){color:#567890}"
                                           "ul:has(input){padding-left:17px}"),
                            {});
  CssComputedStyleEngine engine(contract);
  const QHash<InlineType, QPair<QString, QString>> expected = {{InlineType::Strong, {"strong", "#123456"}},
                                                               {InlineType::Emphasis, {"em", "#234567"}},
                                                               {InlineType::Code, {"code", "#345678"}},
                                                               {InlineType::Strikethrough, {"strike", "#456789"}}};
  int checked = 0;
  for (const auto& node : p->inlines()) {
    if (!expected.contains(node.type()))
      continue;
    const auto* element = builder.buildInline(*p, node.sourceStart() + 2);
    // Text inside nested Markdown spans resolves to its semantic ancestor.
    while (element && element->parent &&
           element->parent->attributes.value("md-inline") != expected.value(node.type()).first)
      element = element->parent;
    require(element && element->parent->tag == "span", QStringLiteral("inline wrapper retains native semantic tag"));
    require(engine.styleFor(*element).resolvedValue("color") == expected.value(node.type()).second,
            QStringLiteral("direct md-inline selector matches parsed inline: ") + expected.value(node.type()).first);
    ++checked;
  }
  require(checked == 4, QStringLiteral("all four inline types are covered"));
  require(engine.styleFor(*builder.build(*list)).resolvedValue("color") == "#567890",
          QStringLiteral("has() finds Markdown child classes"));
  require(engine.styleFor(*builder.build(*list)).length("padding-left").px == 17,
          QStringLiteral("task checkboxes also participate in ancestor has() queries"));
  require(engine.styleFor(prototypes.element("strong")).resolvedValue("color") == "#123456",
          QStringLiteral("prototype and live inline topology agree"));
}

void testVlookStyleAndRaster(const QString& css, const MarkdownDocument& document) {
  auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(css, "vlook-fancy-dark-regression", {}));
  require(theme.paragraphFont().pointSizeF() > 0 && theme.paragraphFont().pointSizeF() < 30,
          QStringLiteral("VLOOK variable font shorthand must not produce a huge dimension"));
  DocumentLayout layout;
  layout.rebuild(document, theme, 960);
  const auto* h1 = findFirstBlock(document.root(), BlockType::Heading);
  const auto* paragraph = firstInlineParagraph(document.root());
  const auto* page = theme.elementStyleForNode(document.root(), "#write");
  const auto* heading = theme.elementStyleForNode(*h1, "h1");
  const auto* prose = theme.elementStyleForNode(*paragraph, "p");
  require(page && heading && prose, QStringLiteral("live page, heading and prose styles exist"));
  const auto* fence = findFirstBlock(document.root(), BlockType::CodeFence);
  const auto label = theme.pseudoForNode(*fence, "before");
  require(label && label->content == "cpp", QStringLiteral("actual VLOOK attr(lang) produces the code language"));
  const auto* alert = findFirstBlock(document.root(), BlockType::BlockQuote);
  const auto* alertBody = layout.block(alert->children().front()->id());
  require(alertBody && alertBody->inlineLayout() &&
              alertBody->inlineLayout()->visibleText().startsWith("NOTE\nA semantic alert."),
          QStringLiteral("actual VLOOK alert displays a semantic title and body"));
  require(alertBody->inlineLayout()->generatedPseudoRects().size() == 1 &&
              alertBody->inlineLayout()->cursorRect(0).left() > 16,
          QStringLiteral("actual VLOOK SVG icon and its half-em margin reserve title flow"));
  const auto images = theme.cssInlineImages(*alert->children().front());
  require(images.size() == 1 && images.front().element->tag == "svg" &&
              images.front().element->parent->classes.contains("md-alert-text-note"),
          QStringLiteral("VLOOK targets the same semantic SVG child as Typora"));
  const auto* table = findFirstBlock(document.root(), BlockType::Table);
  require(layout.block(table->id())->outerCssBox().has_value(),
          QStringLiteral("actual VLOOK figure wrapper participates in layout"));
  require(theme.backgroundColor() == QColor("#1a1915"), QStringLiteral("Fancy Dark page palette"));
  require(prose->paint.color == QColor("#faf9f5"),
          QStringLiteral("Fancy Dark prose inherits foreground through content host"));
  require(prose->text.fontSizePx >= 12 && prose->text.fontSizePx < 30,
          QStringLiteral("prose font remains a readable size"));
  require(heading->paint.color == QColor("#d97757"), QStringLiteral("Fancy Dark heading accent"));
  require(!heading->paint.backgroundLayers.empty() &&
              heading->paint.backgroundLayers.front().image.kind != GradientSpec::Kind::None &&
              heading->box.borderRightWidth > 0,
          QStringLiteral("Fancy Dark heading strip background and edge border"));
  require(!page->paint.backgroundLayers.empty() &&
              page->paint.backgroundLayers.front().bitmap.size() == QSize(120, 120),
          QStringLiteral("real embedded PNG texture is decoded"));
  // A genuine @supports block in this snapshot targets VLOOK's backdrop class.
  DocumentStylePrototypes prototypes;
  auto panel = prototypes.element("p");
  panel.classes << "v-backdrop-blurs";
  CssComputedStyleEngine engine(*theme.documentStyleSheet());
  require(engine.styleFor(panel).resolvedValue("backdrop-filter") == "blur(10px)",
          QStringLiteral("actual VLOOK supports rule is active"));
  require(engine.styleFor(prototypes.element("body")).fontSizePx < 30,
          QStringLiteral("VLOOK body font substitution preserves number/dimension token boundary"));
  int inlineChecks = 0;
  for (const auto& node : paragraph->inlines()) {
    const auto style = theme.inlineStyleForNode(*paragraph, node.sourceStart() + 2);
    if (node.type() == InlineType::Strong) {
      require(style.text.fontWeight >= 600, QStringLiteral("strong text keeps semantic emphasis"));
      ++inlineChecks;
    } else if (node.type() == InlineType::Emphasis) {
      require(style.text.italic, QStringLiteral("emphasis stays italic"));
      ++inlineChecks;
    } else if (node.type() == InlineType::Strikethrough) {
      require(style.text.decorationLines & 4, QStringLiteral("strike decoration reaches inline text"));
      ++inlineChecks;
    } else if (node.type() == InlineType::Code) {
      require(style.paint.color.isValid() && style.box.borderTopWidth > 0,
              QStringLiteral("VLOOK inline code keeps color and chip border"));
      ++inlineChecks;
    } else if (node.type() == InlineType::Link) {
      require(style.paint.color == heading->paint.color, QStringLiteral("links use the Fancy Dark accent"));
      ++inlineChecks;
    }
  }
  require(inlineChecks == 5, QStringLiteral("body regression covers all five semantic inline elements"));

  QImage texture(360, 240, QImage::Format_ARGB32_Premultiplied);
  texture.fill(theme.backgroundColor());
  QPainter texturePainter(&texture);
  auto pageBox = LayoutBox::place("#write", *page, theme.usedBoxForStyle(*page, 360), QRectF(0, 0, 360, 240),
                                  theme.paragraphFont());
  paintLayoutBox(texturePainter, pageBox);
  texturePainter.end();
  int gridPixels = 0;
  for (int y = 20; y < 220; ++y)
    for (int x = 20; x < 340; ++x)
      if (texture.pixelColor(x, y) != theme.backgroundColor())
        ++gridPixels;
  require(gridPixels > 500 && gridPixels < 20000,
          QStringLiteral("grid is painted as a faint repeating texture, not a solid fill"));

  QImage image(960, 900, QImage::Format_ARGB32_Premultiplied);
  image.fill(theme.viewportBackgroundColor());
  QPainter painter(&image);
  const auto rect = layout.pageRect(theme, 900);
  paintLayoutBox(painter, LayoutBox::place("#write", *page, theme.usedBoxForStyle(*page, rect.width()), rect,
                                           theme.paragraphFont()));
  const auto blocks = layout.visibleBlocks(QRectF(0, 0, 960, 900), theme);
  for (const auto* block : blocks)
    block->paint(painter, theme, 0, nullptr);
  painter.end();
  const auto* headingBlock = layout.block(h1->id());
  const auto* paragraphBlock = layout.block(paragraph->id());
  require(headingBlock && headingBlock->rect().width() > 600 && headingBlock->rect().height() > 20,
          QStringLiteral("heading strip spans the document column"));
  require(paragraphBlock && paragraphBlock->inlineLayout() && !paragraphBlock->inlineLayout()->plainText().isEmpty(),
          QStringLiteral("prose and inline content survive live layout"));
  int accentPixels = 0, foregroundPixels = 0;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x) {
      const auto c = image.pixelColor(x, y);
      if (c.red() > 140 && c.red() > c.green() * 1.3 && c.green() > c.blue())
        ++accentPixels;
      if (c.red() > 200 && c.green() > 200 && c.blue() > 200)
        ++foregroundPixels;
    }
  require(accentPixels > 100 && foregroundPixels > 100,
          QStringLiteral("heading accents, body and inline glyphs are actually painted"));
  const QString snapshot =
      QFileInfo(QCoreApplication::applicationFilePath()).absolutePath() + "/vlook-fancy-dark-regression.png";
  require(image.save(snapshot), QStringLiteral("save fixed viewport raster for visual review"));
}
void testVlookAlerts(const QString& css, const QString& markdown) {
  DocumentSession session;
  session.setMarkdownText(markdown, false);
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(css, "vlook-alerts", {}));
  DocumentLayout layout;
  layout.rebuild(session.document(), theme, 960);
  QImage image(960, qCeil(layout.totalHeight() + 20), QImage::Format_ARGB32_Premultiplied);
  image.fill(theme.backgroundColor());
  QPainter painter(&image);
  for (const auto& child : session.document().root().children())
    layout.block(child->id())->paint(painter, theme, 0, nullptr);
  painter.end();
  int count = 0;
  for (const auto& child : session.document().root().children()) {
    require(child->alertKind() != AlertKind::None, "all fixed samples are semantic alerts");
    const auto* paragraph = child->children().front().get();
    const auto* text = layout.block(paragraph->id())->inlineLayout();
    const auto images = theme.cssInlineImages(*paragraph);
    require(images.size() == 1 && text->generatedPseudoRects().size() == 1, "each VLOOK alert has one SVG title icon");
    require(theme.computedStyleForElement(*images[0].element).length("margin-right").px > 0 &&
                text->cursorRect(0).left() > 16,
            "the original VLOOK title SVG margin reserves real space");
    const auto marker = paragraph->inlines().front().text();
    require(text->visibleText().startsWith(marker.mid(2, marker.size() - 3) + "\n"),
            "each VLOOK alert keeps its semantic title and body");
    require(theme.styleForElement(*images[0].element).paint.color == QColor(Qt::white),
            "all VLOOK alert title icons inherit the theme's white foreground");
    ++count;
  }
  require(count == 5, "fixed VLOOK sample exercises all five alert kinds");
  require(
      image.save(QFileInfo(QCoreApplication::applicationFilePath()).absolutePath() + "/vlook-fancy-dark-alerts.png"),
      "save all five VLOOK alerts for review");
}
} // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  require(argc >= 2, QStringLiteral("VLOOK fixture path is required"));
  const QString path = QString::fromLocal8Bit(argv[1]);
  const auto css = readFixture(path);
  QFile fixture(path);
  require(fixture.open(QIODevice::ReadOnly), QStringLiteral("fixture readable"));
  require(QCryptographicHash::hash(fixture.readAll(), QCryptographicHash::Sha256).toHex() ==
              "dc9ce7c464f4ddf8644040fb77c7c7e408a70f1bee3ba2c9f9a8ac746901f3cc",
          QStringLiteral("VLOOK snapshot remains fixed"));
  DocumentSession session;
  session.setMarkdownText(readFixture(QFileInfo(path).absolutePath() + "/sample.md"), false);
  testSemanticContract(session.document());
  testVlookStyleAndRaster(css, session.document());
  testVlookAlerts(css, readFixture(QFileInfo(path).absolutePath() + "/alerts.md"));
  return 0;
}
