#include "document/DocumentSession.h"
#include "document/MarkdownDocument.h"
#include "render/CssPaintAnimation.h"
#include "render/DocumentLayout.h"
#include "render/BlockLayout.h"
#include "render/InlineLayout.h"
#include "theme/CssThemeMapper.h"
#include "theme/CssValueParser.h"
#include "../TestUtils.h"

#include <QApplication>
#include <QImage>
#include <QPainter>
#include <cmath>

using namespace muffin;
namespace {
bool approximately(qreal a, qreal b, qreal tolerance = .01) { return qAbs(a - b) < tolerance; }
std::shared_ptr<const CssPaintRecipe> recipe(const QString& css, const QString& tag = "em") {
  const auto sheet = CssThemeParser::parse(css, {});
  CssComputedStyleEngine engine(sheet);
  CssElement element; element.tag = tag;
  return compileCssPaintRecipe(tag, [&](CssElementState state) { state.subject = &element; return engine.styleFor(element, state); }, sheet);
}
ThemeElementStyle projected(const QString& css) {
  CssElement element; element.tag = "em";
  const auto sheet = CssThemeParser::parse(css, {});
  return CssThemeMapper::projectComputedStyle("em", CssComputedStyleEngine(sheet).styleFor(element));
}
QImage boxImage(const ThemeElementStyle& style, QRectF rect = {10, 10, 84, 30}) {
  QImage image(110, 60, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
  QFont font; font.setPointSizeF(12);
  const auto box = LayoutBox::place("em", style, {}, rect, font);
  QPainter painter(&image); paintLayoutBox(painter, box);
  return image;
}
void testBackgroundUsedValues() {
  const auto style = projected("em{background-image:linear-gradient(90deg,red 50%,transparent 50%);"
      "background-repeat:repeat-x;background-position:0 100%;background-size:6px 3px}");
  const auto image = boxImage(style);
  for (int y = 10; y < 37; ++y) require(image.pixelColor(20, y) == QColor(Qt::white), "stitches must occupy only the bottom three pixels");
  int red = 0;
  for (int x = 10; x < 94; ++x) if (image.pixelColor(x, 38).red() > 240 && image.pixelColor(x, 38).green() < 20) ++red;
  require(red >= 40 && red <= 44, "repeat-x must retain a six pixel period, including transparent gaps");
  const auto zero = projected("em{background:linear-gradient(red,red) 0 bottom / 0 3px no-repeat}");
  require(boxImage(zero).pixelColor(30, 38) == QColor(Qt::white), "zero background dimensions must paint no image");
  const auto shorthand = projected("em{background-position-x:99px;background-size:9px 9px;"
      "background:linear-gradient(red,red) right bottom / 10px 4px no-repeat}");
  const auto positioned = boxImage(shorthand);
  require(positioned.pixelColor(92, 38) == QColor(Qt::red) && positioned.pixelColor(80, 38) == QColor(Qt::white),
          "background shorthand must reset old axes, parse its position/size and avoid repetition");
  const auto edge = parseCssPosition("right 4px bottom 2px");
  require(approximately(edge.first.used(50), 46) && approximately(edge.second.used(20), 18), "edge offsets must remain relative to the available space");
}
void testPerPropertyTransitionsAndReversal() {
  const auto motion = recipe("em{color:black;background-position:0 0;transition:color 100ms linear,background-position 400ms linear 100ms}"
                            "em:hover{color:white;background-position:100% 0}");
  require(bool(motion), "interactive element must compile a recipe");
  CssPaintContext context; context.timeMs = 0; context.sample(*motion, "one", 0);
  context.sample(*motion, "one", 1);
  context.timeMs = 50;
  auto mid = context.sample(*motion, "one", 1);
  require(mid.paint.color.red() >= 126 && mid.paint.color.red() <= 129, "color should have its own duration");
  require(approximately(mid.paint.backgroundPositionX.fraction, 0), "background transition should honor its own delay");
  const auto reversed = context.sample(*motion, "one", 0);
  require(reversed.paint.color == mid.paint.color, "interrupting a transition must start from its current value");
  context.timeMs = 100;
  const auto returning = context.sample(*motion, "one", 0);
  require(returning.paint.color.red() >= 62 && returning.paint.color.red() <= 66, "outgoing transition must continue smoothly");
  context.timeMs = 1000; context.needsFrame = false;
  context.sample(*motion, "one", 0);
  require(!context.needsFrame, "completed transitions must stop requesting frames");
  context.timeMs = 1100; context.sample(*motion, "one", 1);
  context.timeMs = 1300;
  mid = context.sample(*motion, "one", 1);
  require(approximately(mid.paint.backgroundPositionX.fraction, .25), "position should advance independently after its delay");
  require(approximately(cssTimingProgress(.5, "ease"), .8024, .001), "ease must solve CSS cubic-bezier rather than approximate it with a power curve");
  require(approximately(cssTimingProgress(.4, "steps(4,end)"), .25), "step timing must use discrete intervals");
}
void testKeyframesAndIdle() {
  const auto motion = recipe("@keyframes stitchFlow{from{background-position-x:0}to{background-position-x:6px}}"
      "em{background-position:0 100%}em:hover{animation:stitchFlow .5s linear infinite}");
  CssPaintContext context; context.timeMs = 0; context.sample(*motion, "stitch", 1);
  context.timeMs = 125; context.needsFrame = false;
  const auto frame = context.sample(*motion, "stitch", 1);
  require(approximately(frame.paint.backgroundPositionX.px, 1.5) && approximately(frame.paint.backgroundPositionY.fraction, 1),
          "keyframes should animate one background axis without destroying the other");
  require(context.needsFrame, "visible infinite animations should request another frame");
  context.needsFrame = false; context.sample(*motion, "stitch", 0);
  require(!context.needsFrame, "leaving hover must remove its animation and let the driver become idle");
  const auto finite = recipe("@keyframes fade{from{opacity:1}to{opacity:0}}em{animation:fade 200ms linear 2 forwards}");
  context.clear(); context.timeMs = 0; context.sample(*finite, "fade", 0);
  context.timeMs = 100;
  require(approximately(context.sample(*finite, "fade", 0).paint.opacity, .5), "finite animations must play rather than being filtered out");
  context.timeMs = 450; context.needsFrame = false;
  require(approximately(context.sample(*finite, "fade", 0).paint.opacity, 0) && !context.needsFrame,
          "forwards fill should retain the final finite value without a running timer");
}
void testPausedAnimationAndSparseTracks() {
  const auto motion = recipe("@keyframes wave{0%{opacity:0}50%{background-color:red}100%{opacity:1}}"
      "em{animation:wave 1s linear infinite}em:hover{animation-play-state:paused}");
  CssPaintContext context; context.timeMs = 0; context.sample(*motion, "wave", 0);
  context.timeMs = 250;
  require(approximately(context.sample(*motion, "wave", 0).paint.opacity, .25), "properties omitted at a keyframe must use their own track");
  context.sample(*motion, "wave", 1);
  context.timeMs = 1000; context.needsFrame = false;
  require(approximately(context.sample(*motion, "wave", 1).paint.opacity, .25) && !context.needsFrame, "paused animation must freeze without driving repaint");
  context.sample(*motion, "wave", 0);
  context.timeMs = 1250;
  require(approximately(context.sample(*motion, "wave", 0).paint.opacity, .5), "resuming an animation must retain elapsed progress");
}
void testScopedSelectorState() {
  const auto sheet = CssThemeParser::parse("p{color:black}p:hover{color:red}em:hover + strong{background:blue}", {});
  CssComputedStyleEngine engine(sheet);
  CssElement p; p.tag = "p";
  CssElement em; em.tag = "em"; em.parent = &p;
  CssElement strong; strong.tag = "strong"; strong.parent = &p; strong.previousSibling = &em;
  CssElementState state; state.hover = true; state.subject = &strong;
  const auto css = engine.styleFor(strong, state);
  require(css.resolvedValue("color") == "red", "hovered ancestors must supply inherited values");
  require(css.resolvedValue("background-color").isEmpty(), "hovering a subject must not falsely hover its sibling in a selector");
}
void testInlineFragmentsAndPreciseHover() {
  DocumentSession session; session.setMarkdownText("*first* and *second*", false);
  const auto& node = *session.document().root().children().front();
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "p{font-size:16px;color:black}em{background:blue;padding:2px;box-decoration-break:clone}em:hover{background:red}", "test", {}));
  InlineLayout layout; InlineLayout::BuildOptions options; options.styleNode = &node;
  layout.build(node.inlines(), theme, 400, theme.paragraphFont(), options);
  QVector<const LayoutBox*> emphasis;
  for (const auto& box : layout.inlineBoxes()) if (box.hostKey == "em") emphasis.push_back(&box);
  require(emphasis.size() == 2, "separate emphasis elements must have separate semantic boxes");
  const auto first = emphasis[0]->borderBox.translated(10, 10), second = emphasis[1]->borderBox.translated(10, 10);
  CssPaintContext context; context.pointer = first.center(); context.timeMs = 0;
  const auto render = [&] {
    QImage image(450, 90, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
    QPainter painter(&image); layout.paint(painter, {10, 10}, 0, 0, &context, "paragraph"); return image;
  };
  const auto image = render();
  require(image.pixelColor(qFloor(first.left() + 1), qFloor(first.top() + 1)) == QColor(Qt::red), "hover should paint the first inline element");
  require(image.pixelColor(qFloor(second.left() + 1), qFloor(second.top() + 1)) == QColor(Qt::blue), "hover must not affect the other emphasis on the same line");
  context.pointer.reset(); context.timeMs = 20;
  const auto left = render();
  require(left.pixelColor(qFloor(first.left() + 1), qFloor(first.top() + 1)) == QColor(Qt::blue), "pointer leave must restore the base inline background");
  const auto cursor = layout.cursorRectForSourceOffset(3);
  render();
  require(layout.cursorRectForSourceOffset(3) == cursor, "paint state changes must never move the caret or reshape text");
}
void testNestedWrappedInlineBackgrounds() {
  DocumentSession session; session.setMarkdownText("*one **two** three four five six seven eight nine*", false);
  const auto& node = *session.document().root().children().front();
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "p{font-size:16px}em{background:yellow;padding:2px;box-decoration-break:clone}strong{background:red}", "test", {}));
  InlineLayout layout; InlineLayout::BuildOptions options; options.styleNode = &node;
  layout.build(node.inlines(), theme, 120, theme.paragraphFont(), options);
  int emphasis = 0, strong = 0;
  for (const auto& box : layout.inlineBoxes()) {
    if (box.hostKey == "em") {
      ++emphasis;
      require(box.usedBox.padding.left() > 0 && box.usedBox.padding.right() > 0, "clone decoration must retain both edges on every wrapped fragment");
    }
    if (box.hostKey == "strong") ++strong;
  }
  require(emphasis >= 2 && strong >= 1, "nested inline backgrounds must retain outer and inner boxes across wrapping");
}
void testInlineCompositeAndInteractionStates() {
  DocumentSession session; session.setMarkdownText("*first **nested** end* and *second*", false);
  const auto& node = *session.document().root().children().front();
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "p{font-size:16px;color:black}em{color:red;background:blue;padding:3px;opacity:.5}"
      "strong{color:lime}em:hover{filter:grayscale(1);transform:translateY(6px)}"
      "em:focus{background:yellow}em:active{background:lime}", "test", {}));
  InlineLayout layout; InlineLayout::BuildOptions options; options.styleNode = &node;
  layout.build(node.inlines(), theme, 500, theme.paragraphFont(), options);
  QVector<QRectF> emphasis;
  for (const auto& box : layout.inlineBoxes()) if (box.hostKey == "em") emphasis << box.borderBox.translated(10, 10);
  require(emphasis.size() == 2, "composite fixture needs two independent emphasis elements");
  CssPaintContext context; context.timeMs = 0;
  const auto render = [&](qsizetype focused = -1) {
    QImage image(550, 100, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
    QPainter painter(&image); layout.paint(painter, {10, 10}, 0, 0, &context, "paragraph", focused); return image;
  };
  const auto initial = render();
  const auto padding = emphasis.front().topLeft() + QPointF(1, 1);
  const auto blue = initial.pixelColor(padding.toPoint());
  require(blue.blue() > 250 && blue.red() >= 126 && blue.red() <= 129,
          "inline opacity must composite the complete element exactly once");
  context.pointer = emphasis.front().center();
  const auto hovered = render();
  const auto gray = hovered.pixelColor((padding + QPointF(0, 6)).toPoint());
  require(qAbs(gray.red() - gray.green()) <= 1 && qAbs(gray.green() - gray.blue()) <= 1 && gray.red() < 240,
          "hover transform and grayscale must move and filter the inline background");
  int chromatic = 0;
  QRect chromaticBounds;
  const auto area = emphasis.front().translated(0, 6).toAlignedRect().intersected(hovered.rect());
  for (int y = area.top(); y <= area.bottom(); ++y) for (int x = area.left(); x <= area.right(); ++x) {
    const auto color = hovered.pixelColor(x, y);
    if (qAbs(color.red() - color.green()) > 2 || qAbs(color.green() - color.blue()) > 2) {
      ++chromatic; chromaticBounds = chromaticBounds.united(QRect(x, y, 1, 1));
    }
  }
  require(chromatic == 0, QString("inline grayscale must include its own and nested glyphs: %1 at %2,%3,%4,%5 (area %6,%7,%8,%9)")
      .arg(chromatic).arg(chromaticBounds.x()).arg(chromaticBounds.y()).arg(chromaticBounds.width()).arg(chromaticBounds.height())
      .arg(area.x()).arg(area.y()).arg(area.width()).arg(area.height()));
  require(hovered.pixelColor((emphasis[1].topLeft() + QPointF(1, 1)).toPoint()) == blue,
          "compositing the hovered element must leave its sibling untouched");
  context.pointer.reset();
  const auto focused = render(3);
  const auto yellow = focused.pixelColor(padding.toPoint());
  require(yellow.red() > 250 && yellow.green() > 250 && yellow.blue() < 150,
          "caret focus must target its semantic inline element");
  context.pointer = emphasis[1].center(); context.pressed = true;
  const auto pressed = render();
  const auto active = pressed.pixelColor((emphasis[1].topLeft() + QPointF(1, 7)).toPoint());
  require(qAbs(active.red() - active.green()) <= 1 && active.red() < 240,
          "active state must compose with hover on the pressed element");
}
QRect blueBounds(const QImage& image) {
  QRect result;
  for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
    const auto color = image.pixelColor(x, y);
    if (color.blue() > 220 && color.red() < 40 && color.green() < 40) result = result.united(QRect(x, y, 1, 1));
  }
  return result;
}
void testHostAndPseudoPropertyTimelines() {
  DocumentSession session; session.setMarkdownText("# Heading", false);
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "#write{margin:0;padding:0}h1{margin:0;padding:10px;position:relative;color:black;transition:color 100ms linear,transform 400ms linear}"
      "h1:hover{color:red;transform:translateY(-2px)}"
      "h1::after{content:'';position:absolute;left:50%;bottom:0;width:40px;height:4px;background:blue;transform:translateX(-50%);transition:width 800ms linear}"
      "h1:hover::after{width:100%}", "test", {}));
  DocumentLayout layout; layout.rebuild(session.document(), theme, 500);
  const auto* block = layout.block(session.document().root().children().front()->id());
  require(block, "heading should be laid out");
  CssPaintContext context; context.timeMs = 0;
  const auto render = [&] {
    QImage image(550, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
    QPainter painter(&image);
    BlockLayout::BlockPaintState state; state.css = &context;
    block->paint(painter, theme, 0, nullptr, state); return image;
  };
  const auto initial = blueBounds(render());
  require(initial.width() >= 39 && initial.width() <= 41, "base positioned pseudo should retain its authored width");
  context.pointer = block->cssBorderBox().center(); render();
  context.timeMs = 200;
  const auto mid = blueBounds(render());
  const qreal expected = 40 + (block->cssBoxGeometry().paddingBox.width() - 40) * .25;
  require(approximately(mid.width(), expected, 2), "pseudo width must use its own 800ms transition rather than the host duration");
  require(approximately(mid.center().x(), initial.center().x(), 1.1), QStringLiteral("percentage translation should preserve centering: initial %1,%2 mid %3,%4")
      .arg(initial.x()).arg(initial.width()).arg(mid.x()).arg(mid.width()));
  context.timeMs = 1000; context.needsFrame = false; render();
  require(!context.needsFrame, "host and pseudo timelines must both become idle after completion");
}
void testMotionShorthandCascadeAndKeyframeExpansion() {
  const auto motion = recipe("em{color:black;transition-duration:8s;transition:color 100ms linear}em:hover{color:white}");
  CssPaintContext context; context.timeMs = 0; context.sample(*motion, "one", 1);
  context.timeMs = 50;
  require(context.sample(*motion, "one", 1).paint.color.red() >= 126,
          "later transition shorthand must reset a previously authored duration longhand");
  const auto expanded = recipe("@keyframes slide{from{background-position:0% 0%}to{background-position:100% 100%}}"
      "em{background-position-x:10px;background-position-y:20px;animation:slide 1s linear forwards}");
  context.clear(); context.timeMs = 0; context.sample(*expanded, "slide", 0);
  context.timeMs = 500;
  const auto mid = context.sample(*expanded, "slide", 0);
  require(approximately(mid.paint.backgroundPositionX.fraction, .5) && approximately(mid.paint.backgroundPositionY.fraction, .5),
          "keyframe shorthand must expand both axes through the common declaration pipeline");
}
void testElementCompositingAndPointerCoordinates() {
  QImage image(100, 60, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
  QPainter painter(&image);
  painter.translate(4, 2);
  ThemeElementPaintStyle style; style.opacity = .5;
  CssPaintContext context; context.pointer = QPointF(20, 20);
  paintCssElement(painter, {10, 10, 40, 30}, {10, 10, 40, 30}, style, 1, &context,
      [&](QPainter& target, const ThemeElementPaintStyle& paint) {
    require(paint.opacity == 1, "layer opacity must be applied once to the composite");
    require(target.worldTransform().inverted().map(*context.pointer) == QPointF(16, 18), "offscreen layers must retain semantic pointer coordinates");
    target.fillRect(QRectF(10, 10, 40, 30), Qt::blue);
    target.fillRect(QRectF(12, 12, 20, 20), Qt::red);
  });
  require(context.pointer == QPointF(20, 20), "layer rendering must restore the view's pointer");
  const auto pixel = image.pixelColor(20, 20);
  require(pixel.red() > 250 && pixel.green() >= 126 && pixel.blue() >= 126, "opacity should composite the red content with the page, without leaking the blue background through it");
}
void testPseudoFilterAffectsGlyphs() {
  DocumentSession session; session.setMarkdownText("> quote", false);
  const auto theme = RenderTheme::fromDefinition(CssThemeMapper::fromCss(
      "#write{padding:20px;margin:0}blockquote{position:relative;margin:0;padding:25px}"
      "blockquote::before{content:'A';position:absolute;top:0;left:0;color:red;font-size:20px;filter:grayscale(1);transition:filter 200ms linear}"
      "blockquote:hover::before{filter:grayscale(0)}", "test", {}));
  const auto* element = theme.cssElementForNode(*session.document().root().children().front());
  const auto motion = theme.paintRecipeForElement(*element, "before");
  require(motion && approximately(motion->states[0].paint.filterGrayscale, 1), "pseudo base filter must survive computed-style projection");
  require(approximately(motion->states[1].paint.filterGrayscale, 0), "pseudo hover filter must have its own state");
  DocumentLayout layout; layout.rebuild(session.document(), theme, 500);
  const auto* block = layout.block(session.document().root().children().front()->id());
  const auto pseudoRects = block->positionedPseudoRects();
  require(!pseudoRects.isEmpty(), "quote should expose a positioned pseudo glyph");
  const auto pseudoArea = pseudoRects.front().adjusted(-2, -2, 2, 2).toAlignedRect();
  CssPaintContext context; context.timeMs = 0;
  const auto redPixels = [&] {
    QImage image(550, 200, QImage::Format_ARGB32_Premultiplied); image.fill(Qt::white);
    QPainter painter(&image); BlockLayout::BlockPaintState state; state.css = &context;
    block->paint(painter, theme, 0, nullptr, state); painter.end();
    int red = 0;
    const auto area = pseudoArea.intersected(image.rect());
    for (int y = area.top(); y <= area.bottom(); ++y) for (int x = area.left(); x <= area.right(); ++x) {
      const auto color = image.pixelColor(x, y); if (color.red() > color.green() + 40 && color.red() > color.blue() + 40) ++red;
    }
    return red;
  };
  require(redPixels() == 0, "pseudo filter should affect its glyphs as well as its background");
  context.pointer = block->cssBorderBox().center(); redPixels(); context.timeMs = 250;
  require(redPixels() > 10, "hover filter animation should restore the pseudo glyph's color");
}
}  // namespace
int main(int argc, char** argv) {
  QApplication::setAttribute(Qt::AA_Use96Dpi);
  QApplication app(argc, argv);
  runTest("background used values", testBackgroundUsedValues);
  runTest("per-property transitions and reversal", testPerPropertyTransitionsAndReversal);
  runTest("keyframes and idle", testKeyframesAndIdle);
  runTest("paused animation and sparse tracks", testPausedAnimationAndSparseTracks);
  runTest("scoped selector state", testScopedSelectorState);
  runTest("inline fragments and precise hover", testInlineFragmentsAndPreciseHover);
  runTest("nested wrapped backgrounds", testNestedWrappedInlineBackgrounds);
  runTest("inline compositing and interaction states", testInlineCompositeAndInteractionStates);
  runTest("host and pseudo property timelines", testHostAndPseudoPropertyTimelines);
  runTest("motion shorthand cascade and keyframe expansion", testMotionShorthandCascadeAndKeyframeExpansion);
  runTest("element compositing and pointer coordinates", testElementCompositingAndPointerCoordinates);
  runTest("pseudo filter affects glyphs", testPseudoFilterAffectsGlyphs);
  return 0;
}
