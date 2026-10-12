#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math {
using namespace mathml;
namespace {

struct RootBoxContext {
  const MathLayoutResult& layout;
  const MathRenderNode* semantic;
  qreal scale;
  qreal cssRootFontPixelSize;
  qreal scaledWidth;
  qreal scaledHeight;
};

void measureFraction(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  const auto& cssRootFontPixelSize = context.cssRootFontPixelSize;
  const qreal fractionHeight = fractionMathMlHeight(
      semantic, cssRootFontPixelSize, scale);
  if (fractionHeight > 0.0) {
    root.width = fractionCssWidth(semantic, scale);
    root.advance = root.width;
    root.height = fractionHeight;
  } else {
    root.width = fractionCssWidth(semantic, scale);
    root.height = std::ceil(layout.naturalSize.height() * scale);
  }
  root.advance = root.width;
  if (enclosingKind(layout.root.get(), semantic, MathRenderKind::LeftRight)) {
    root.width = cssNodeWidth(layout.root.get(), scale);
    root.advance = root.width;
  }
  return;
}

void measureRadical(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  const auto& scaledHeight = context.scaledHeight;
  const OpenTypeMathFont& mathFont = OpenTypeMathFont::instance();
  const MathFontConstants& constants = mathFont.constants();
  const MathRenderNode* radicalBox = semantic->radicalIndex
      ? indexedRadicalBox(semantic) : semantic;
  const MathRenderNode* degree = semantic->radicalIndex
      ? indexedRadicalDegree(semantic, radicalBox) : nullptr;
  const qreal degreeAdvance = degree
      ? std::max<qreal>(0.0, constants.radicalKernBeforeDegree +
                                radicalDegreeAdvanceWidth(degree, scale) +
                                constants.radicalKernAfterDegree)
      : 0.0;
  const MathRenderNode* body = radicalBody(radicalBox);
  const GlyphInkExtents bodyInk = glyphInkExtents(body, 1.0);
  const qreal radicalPadding = constants.radicalVerticalGap +
                               constants.radicalRuleThickness;
  const qreal recursiveNaturalHeight =
      hasNestedSemantic(body, MathSemanticKind::SupSub) ||
              hasNestedSemantic(body, MathSemanticKind::Radical)
          ? embeddedBraceStyleReduction(body) > 0.0
                ? 0.0 : scaledHeight
          : 0.0;
  const qreal requiredExtent = std::max({
      bodyInk.top + bodyInk.bottom + radicalPadding,
      cssNodeHeight(body, scale) + radicalPadding,
      root.height, recursiveNaturalHeight}) +
      (middleDelimiterMarker(body)
           ? kChromiumMathAxisOffsetPx : 0.0);
  const auto heightVariant = mathFont.verticalVariant(QString(QChar(0x221A)), requiredExtent);
  root.width = radicalCssWidth(semantic, scale);
  root.advance = heightVariant
      ? cssNodeWidth(body, scale) + heightVariant->advance - constants.spaceAfterScript +
            (degree ? constants.radicalKernBeforeDegree + degreeAdvance
                    : 0.0)
      : root.width;
  if (heightVariant) {
    qreal extra = (!primarySemanticNode(body) &&
                   !middleDelimiterMarker(body)) ||
                          semantic->radicalIndex ||
                          heightVariant->extent > 30.0 ||
                          containsTextModeRun(body)
        ? constants.radicalExtraAscender
        : constants.radicalRuleThickness / 2.0;
    if (nestedSemanticNode(semantic, MathSemanticKind::Array))
      extra += constants.radicalExtraAscender -
               constants.radicalRuleThickness / 2.0;
    root.height = std::max(root.height, heightVariant->extent + extra);
  }
  if (root.height == 0.0) root.height = std::ceil(scaledHeight);
  if (degree && heightVariant) {
    const qreal degreeScale = constants.scriptScriptPercentScaleDown;
    qreal degreeHeight = radicalDegreePaintHeight(degree, scale);
    if (!primarySemanticNode(degree))
      degreeHeight = std::round(degreeHeight * degreeScale);
    const qreal degreeBottom = root.height -
        constants.radicalDegreeBottomRaisePercent *
            heightVariant->extent;
    const qreal topOverhang = std::max<qreal>(
        0.0, degreeHeight - degreeBottom);
    root.height = snapLayoutUnit(
        root.height + std::max<qreal>(
            0.0, 2.0 * topOverhang -
                     constants.radicalRuleThickness / 2.0));
  }
  if (enclosingKind(layout.root.get(), semantic,
                    MathRenderKind::LeftRight)) {
    root.width = cssNodeWidth(layout.root.get(), scale);
    root.advance = root.width;
  }
  return;
}

void measureSupSub(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  const auto& cssRootFontPixelSize = context.cssRootFontPixelSize;
  const auto& scaledWidth = context.scaledWidth;
  const auto& scaledHeight = context.scaledHeight;
  if (const MathRenderNode* brace = semantic->kind == MathRenderKind::VList
          ? firstKind(semantic, MathRenderKind::Accent) : nullptr;
      brace && (brace->accentKind == MathAccentKind::UnderBrace ||
                brace->accentKind == MathAccentKind::OverBrace)) {
    root.width = cssNodeWidth(brace, scale);
    for (const auto& child : semantic->children)
      if (child && !containsNode(child.get(), brace))
        root.width = std::max(root.width,
                              cssNodeWidth(child.get(), scale));
    root.advance = root.width;
    root.height = braceAccentCssHeight(semantic, brace, scale);
    return;
  }
  root.width = scaledWidth;
  if (semantic && semantic->children.size() == 2) {
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    const qreal base = cssNodeWidth(semantic->children.front().get(), scale);
    const qreal scripts = cssNodeWidth(semantic->children.back().get(), scale);
    root.width = snapEighth(base + scripts + constants.spaceAfterScript);
  }
  if (semantic && hasAtomClass(semantic, QLatin1StringView("mop")))
    root.width += 2.0 * OpenTypeMathFont::instance().constants().spaceAfterScript;
  root.advance = root.width;
  {
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    if (semantic->children.size() >= 3 &&
        hasAtomClass(semantic, QLatin1StringView("mop"))) {
      if (containsTextModeRun(semantic)) {
        if (const auto operation = buildScriptOperation(
                semantic, layout.root.get(),
                QRectF(0.0, 0.0, root.width, root.height), scale)) {
          root.width = operation->container.width() +
                       2.0 * operation->container.left();
          root.advance = root.width;
          root.height = operation->lineAscent + operation->lineDescent;
          return;
        }
      }
      GlyphInkExtents sub = glyphInkExtents(
          semantic->children.front().get(), constants.scriptPercentScaleDown);
      GlyphInkExtents sup = glyphInkExtents(
          semantic->children.back().get(), constants.scriptPercentScaleDown);
      const qreal operatorHeight = std::ceil(
          maxLargeOperatorExtent(semantic->children[1].get()));
      sub.top = std::max(sub.top, std::round(2.0 * constants.axisHeight));
      sup.top = std::max(
          sup.top,
          std::round(2.0 * constants.axisHeight *
                     constants.scriptPercentScaleDown));
      const qreal upperGap = std::max(constants.upperLimitGapMin,
                                      constants.upperLimitBaselineRiseMin);
      const qreal lowerGap = std::max(
          constants.lowerLimitGapMin,
          constants.lowerLimitBaselineDropMin - sub.top);
      root.height = sup.top + sup.bottom + upperGap + operatorHeight +
                    lowerGap + sub.top + sub.bottom;
      return;
    }
    if (semantic->children.size() == 2 &&
        hasAtomClass(semantic, QLatin1StringView("mop"))) {
      root.height = maxLargeOperatorExtent(semantic);
      if (root.scriptKind != MathScriptKind::SubSup)
        root.height = std::ceil(root.height);
      if (root.scriptKind == MathScriptKind::Superscript ||
          root.scriptKind == MathScriptKind::SubSup)
        root.height += constants.superscriptBaselineDropMax;
      if (root.scriptKind == MathScriptKind::Subscript ||
          root.scriptKind == MathScriptKind::SubSup)
        root.height += constants.subscriptBaselineDropMin;
      if (semantic->operatorKind == MathOperatorKind::Limits) {
        const qreal siblingHeight = siblingSemanticHeight(
            layout.root.get(), semantic, scale, cssRootFontPixelSize);
        if (siblingHeight > 0.0)
          root.height = std::max(
              root.height, siblingHeight + constants.spaceAfterScript);
      }
      return;
    }
    const GlyphInkExtents base = glyphInkExtents(
        semantic->children.empty() ? nullptr : semantic->children.front().get(), 1.0);
    const MathRenderNode* scripts = semantic->children.size() > 1
        ? semantic->children.back().get() : nullptr;
    GlyphInkExtents superscript;
    GlyphInkExtents subscript;
    if (root.scriptKind == MathScriptKind::SubSup && scripts && scripts->children.size() >= 2) {
      subscript = glyphInkExtents(scripts->children.front().get(),
                                  constants.scriptPercentScaleDown);
      superscript = glyphInkExtents(scripts->children.back().get(),
                                    constants.scriptPercentScaleDown);
    } else if (root.scriptKind == MathScriptKind::Subscript) {
      subscript = glyphInkExtents(scripts, constants.scriptPercentScaleDown);
    } else {
      superscript = glyphInkExtents(scripts, constants.scriptPercentScaleDown);
    }
    qreal subShift = constants.subscriptShiftDown;
    if (root.scriptKind == MathScriptKind::SubSup) {
      const qreal gap = (constants.superscriptShiftUp - superscript.bottom) -
                        (subscript.top - subShift);
      subShift += std::max<qreal>(0.0, constants.subSuperscriptGapMin - gap);
    }
    const qreal top = root.scriptKind == MathScriptKind::Subscript
        ? base.top
        : std::max(base.top, constants.superscriptShiftUp + superscript.top);
    const qreal bottom = root.scriptKind == MathScriptKind::Superscript
        ? base.bottom
        : std::max(base.bottom, subShift + subscript.bottom);
    root.height = top + bottom;
  }
  if (semantic && containsTextModeRun(semantic) &&
      semantic->operatorKind != MathOperatorKind::Limits &&
      !firstKind(semantic, MathRenderKind::Accent)) {
    if (const auto operation = buildScriptOperation(
            semantic, layout.root.get(),
            QRectF(0.0, 0.0, root.width, root.height), scale))
      root.height = operation->lineAscent + operation->lineDescent;
  }
  if (semantic && semantic->operatorKind == MathOperatorKind::Limits) {
    const qreal siblingHeight = siblingSemanticHeight(
        layout.root.get(), semantic, scale, cssRootFontPixelSize);
    if (siblingHeight > 0.0)
      root.height = std::max(
          root.height,
          siblingHeight + OpenTypeMathFont::instance().constants().spaceAfterScript);
  }
  if (semantic &&
      (hasNestedSemantic(semantic, MathSemanticKind::Fraction) ||
       hasNestedSemantic(semantic, MathSemanticKind::Radical))) {
    root.height = std::max(root.height, scaledHeight);
    root.height = std::max(root.height, cssNodeHeight(semantic, scale));
    if (semantic->children.size() == 2 &&
        root.scriptKind == MathScriptKind::Subscript) {
      const MathFontConstants& constants =
          OpenTypeMathFont::instance().constants();
      const MathRenderNode* baseNode = semantic->children.front().get();
      const MathRenderNode* scriptNode = semantic->children.back().get();
      const GlyphInkExtents baseInk = glyphInkExtents(baseNode, 1.0);
      const GlyphInkExtents scriptInk = glyphInkExtents(
          scriptNode, constants.scriptPercentScaleDown);
      const qreal subShift = std::max({
          constants.subscriptShiftDown,
          scriptInk.top - constants.subscriptTopMax,
          constants.subscriptBaselineDropMin + baseInk.bottom});
      const qreal scriptTop = baseInk.top + subShift - scriptInk.top;
      root.height = std::max(
          root.height, scriptTop + cssNodeHeight(scriptNode, scale));
      root.height = snapLayoutUnit(root.height);
    }
  }
  if (semantic && embeddedBraceStyleReduction(semantic) > 0.0) {
    if (const auto operation = buildScriptOperation(
            semantic, layout.root.get(),
            QRectF(0.0, 0.0, root.width, root.height), scale)) {
      QRectF extent = operation->base;
      for (const QRectF component : {operation->superscript,
                                     operation->subscript})
        if (!component.isEmpty()) extent = extent.united(component);
      root.height = std::max(root.height, extent.height());
      if (semantic->scriptKind == MathScriptKind::Superscript &&
          semantic->children.size() == 2) {
        const MathFontConstants& constants =
            OpenTypeMathFont::instance().constants();
        root.height = std::max(
            root.height,
            cssNodeHeight(semantic->children.front().get(), scale) +
                cssNodeHeight(semantic->children.back().get(), scale) -
                constants.superscriptBottomMin -
                constants.overbarRuleThickness / 4.0);
      }
    }
  }
  return;
}

void measureArray(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  const auto& cssRootFontPixelSize = context.cssRootFontPixelSize;
  root.width = arrayCssWidth(semantic, scale);
  if (firstKind(layout.root.get(), MathRenderKind::LeftRight))
    root.width = cssNodeWidth(layout.root.get(), scale);
  root.advance = root.width - cssRootFontPixelSize * 0.4;
  root.height = arrayCssHeight(semantic, scale);
  if (root.height == 0.0) {
    for (size_t row = 0; row < semantic->arrayRowHeights.size(); ++row) {
      const bool inkDescender = row < semantic->arrayRowInkDescenders.size() &&
                                semantic->arrayRowInkDescenders[row];
      root.height += cssRootFontPixelSize *
          (kMathTableRowHeightEm +
           (inkDescender ? kMathTableDescenderExpansionEm : 0.0));
    }
  }
  return;
}

void measureNone(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& scale = context.scale;
  const auto& scaledWidth = context.scaledWidth;
  if (wrappedTextModeRun(layout.root.get())) {
    root.width = cssNodeWidth(layout.root.get(), scale);
    root.height = cssNodeHeight(layout.root.get(), scale);
  } else if (symbolCount(layout.root.get()) == 1 &&
      !firstKind(layout.root.get(), MathRenderKind::Accent)) {
    const MathRenderNode* symbol = singleSymbol(layout.root.get());
    const auto glyph = nativeGlyphBox(symbol);
    if (glyph) {
      root.width = glyph->width;
      root.height = glyph->height;
    } else if (symbol && symbol->text.size() > 1 &&
               symbol->fontClass == QLatin1String("main")) {
      root.width = cssNodeWidth(layout.root.get(), scale);
      root.height = cssNodeHeight(layout.root.get(), scale);
    } else {
      root.width = std::round(scaledWidth);
      root.height = std::ceil((layout.root->children.empty()
          ? layout.root->height + layout.root->depth
          : layout.root->children.front()->height + layout.root->children.front()->depth) * scale);
    }
  } else {
    root.width = snapEighth(cssNodeWidth(layout.root.get(), scale));
    root.height = (layout.root->children.empty()
        ? layout.root->height + layout.root->depth
        : layout.root->children.front()->height + layout.root->children.front()->depth) * scale;
    root.height = std::ceil(root.height);
    if (hasAtomClass(layout.root.get(), QLatin1StringView("mbin")) &&
        !hasMathNumberToken(layout.root.get()))
      root.height += 1.0;
    root.height = std::max(root.height,
                           std::ceil(maxLargeOperatorExtent(layout.root.get())));
    if (const MathRenderNode* accent = firstKind(layout.root.get(), MathRenderKind::Accent);
        accent && accent->children.size() >= 2) {
      const bool bodyIsLast = accent->accentKind == MathAccentKind::Under ||
                              accent->accentKind == MathAccentKind::OverBrace;
      const MathRenderNode* body = bodyIsLast ? accent->children.back().get()
                                              : accent->children.front().get();
      const qreal bodyWidth = cssNodeWidth(body, scale);
      if ((accent->accentKind == MathAccentKind::Over ||
           accent->accentKind == MathAccentKind::Under) &&
          !accent->accentCharacter.isEmpty() &&
          symbolCount(layout.root.get()) > symbolCount(accent)) {
        const HorizontalAccentSelection widthSelection =
            selectHorizontalAccent(accent->accentCharacter, bodyWidth);
        const qreal accentWidth = widthSelection.text
            ? widthSelection.text->advance
            : widthSelection.fixed ? widthSelection.fixed->extent
            : widthSelection.assembly ? widthSelection.assembly->extent
                                      : bodyWidth;
        root.width += std::max<qreal>(0.0, accentWidth - bodyWidth);
      }
      qreal bodyHeight = std::ceil((body->height + body->depth) * scale);
      bool shapedTextBody = false;
      if (accent->accentKind == MathAccentKind::Under) {
        const GlyphInkExtents bodyInk = glyphInkExtents(body, 1.0);
        bodyHeight = bodyInk.top + bodyInk.bottom;
      }
      if (const MathRenderNode* textRun = wrappedTextModeRun(body)) {
        QString text;
        if (collectTextModeRun(textRun, &text)) {
          if (const auto shaped = shapeTextModeRun(
                  text, mathStyleScale(textRun))) {
            bodyHeight = shapedTextCssHeight(*shaped);
            root.width = shaped->advance;
            shapedTextBody = true;
          }
        }
      } else if (symbolCount(body) == 1) {
        const MathRenderNode* bodySymbol = singleSymbol(body);
        if (const auto glyph = nativeGlyphBox(bodySymbol)) {
          bodyHeight = glyph->height;
        } else if (bodySymbol && bodySymbol->text.size() > 1 &&
                   bodySymbol->fontClass == QLatin1String("main")) {
          if (const auto shaped = OpenTypeMathFont::instance()
                                      .shapeMathMlText(
                                          bodySymbol->text,
                                          mathStyleScale(bodySymbol))) {
            bodyHeight = std::ceil(shaped->inkBounds.height());
            root.width = shaped->advance;
            shapedTextBody = true;
          }
        }
      } else if (hasAtomClass(body, QLatin1StringView("mbin"))) {
        bodyHeight += 1.0;
      }
      if (accent->accentKind == MathAccentKind::Underline &&
          hasMathMlOperatorOverflow(body))
        bodyHeight += std::floor(
            OpenTypeMathFont::instance().constants().fractionRuleThickness);
      const MathRenderNode* accentPart = bodyIsLast
          ? accent->children.front().get() : accent->children.back().get();
      qreal accentHeight = 0.0;
      if (accent->accentKind == MathAccentKind::Overline)
        accentHeight = OpenTypeMathFont::instance().constants().fractionRuleThickness;
      if (accent->accentKind == MathAccentKind::Over ||
          accent->accentKind == MathAccentKind::Under) {
        if (const MathRenderNode* stretchy = firstKind(
                accentPart, MathRenderKind::Stretchy))
          accentHeight = accent->accentKind == MathAccentKind::Under
              ? std::floor((stretchy->height + stretchy->depth) * scale)
              : std::ceil((stretchy->height + stretchy->depth) * scale);
      }
      if (const MathRenderNode* symbol = singleSymbol(accentPart)) {
        if (const auto glyph = OpenTypeMathFont::instance().glyph(symbol->text))
          accentHeight = accent->accentKind == MathAccentKind::Under
              ? std::floor(glyph->inkBounds.height())
              : std::ceil(glyph->inkBounds.height());
      } else if (const MathRenderNode* stretchy = firstKind(accentPart, MathRenderKind::Stretchy);
                 stretchy && stretchy->pathName == QLatin1String("vec")) {
        if (const auto glyph = OpenTypeMathFont::instance().glyph(QString(QChar(0x20D7))))
          accentHeight = std::ceil(glyph->inkBounds.height());
      }
      if ((accent->accentKind == MathAccentKind::Over ||
           accent->accentKind == MathAccentKind::Under) &&
          !accent->accentUsesNaturalWidth &&
          !accent->accentCharacter.isEmpty()) {
        const HorizontalAccentSelection selection =
            selectHorizontalAccent(accent->accentCharacter, root.width);
        accentHeight = selection.height();
        if (selection.text)
          root.width = std::max(root.width, selection.text->advance);
      }
      root.height = bodyHeight + accentHeight +
                    OpenTypeMathFont::instance().constants().fractionRuleThickness;
      if (accent->accentKind == MathAccentKind::Under &&
          symbolCount(layout.root.get()) > symbolCount(accent)) {
        root.height += std::round(
            2.0 * OpenTypeMathFont::instance().constants()
                      .underbarRuleThickness);
      }
      if (shapedTextBody) {
        root.height = cssNodeHeight(accent, scale);
      }
    }
  }
  root.advance = root.width;
  return;
}

void includeEnclosingAccent(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  if (const MathRenderNode* accent = firstKind(
          layout.root.get(), MathRenderKind::Accent);
      semantic && accent && containsNode(accent, semantic) &&
      (accent->accentKind == MathAccentKind::Over ||
       accent->accentKind == MathAccentKind::Under) &&
      !accent->accentCharacter.isEmpty() && accent->children.size() >= 2) {
    const bool bodyIsLast = accent->accentKind == MathAccentKind::Under;
    const MathRenderNode* body = bodyIsLast
        ? accent->children.back().get() : accent->children.front().get();
    root.width = snapEighth(cssNodeWidth(layout.root.get(), scale));
    root.advance = root.width;
    const HorizontalAccentSelection selection = selectHorizontalAccent(
        accent->accentCharacter, root.width);
    const qreal operatorHeight = selection.height();
    const MathFontConstants& constants =
        OpenTypeMathFont::instance().constants();
    root.height = accentBodyCssHeight(body, scale) + operatorHeight +
        (accent->accentKind == MathAccentKind::Under
             ? constants.underbarExtraDescender
             : constants.overbarExtraAscender);
  }
}

void includeSiblingContent(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  const auto& cssRootFontPixelSize = context.cssRootFontPixelSize;
  const auto& scaledHeight = context.scaledHeight;
  const bool hasRootSiblingContent = semantic &&
      symbolCount(layout.root.get()) > symbolCount(semantic);
  if (hasRootSiblingContent) {
    root.width = cssNodeWidth(layout.root.get(), scale);
    const bool accentOwner =
        firstKind(semantic, MathRenderKind::Accent) != nullptr;
    if (root.semanticKind == MathSemanticKind::SupSub &&
        hasAtomClass(semantic, QLatin1StringView("mop")) &&
        semantic->operatorKind != MathOperatorKind::Limits &&
        cssNodeOffset(layout.root.get(), semantic, scale).value_or(0.0) <=
            0.001 &&
        !enclosingKind(layout.root.get(), semantic,
                       MathRenderKind::LeftRight))
      root.width -= OpenTypeMathFont::instance().constants().spaceAfterScript;
    root.advance = root.width;
    const qreal siblingHeight = siblingSemanticHeight(
        layout.root.get(), semantic, scale, cssRootFontPixelSize);
    const bool primaryOwnsHeight = root.height >= siblingHeight;
    root.height = std::max(root.height, siblingHeight);
    if (root.semanticKind == MathSemanticKind::SupSub &&
        !accentOwner &&
        semantic->operatorKind != MathOperatorKind::Limits &&
        !hasAtomClass(semantic, QLatin1StringView("mop")) &&
        primaryOwnsHeight &&
        hasSiblingAtomClass(layout.root.get(), semantic,
                            QLatin1StringView("mbin")))
      root.height += std::ceil(
          OpenTypeMathFont::instance().constants().fractionRuleThickness /
          2.0);
    const MathRenderNode* rowAccent = accentOwner
        ? firstKind(semantic, MathRenderKind::Accent) : nullptr;
    if (rowAccent && rowAccent->accentKind == MathAccentKind::OverBrace) {
      root.height = std::max(
          root.height,
          scaledHeight - OpenTypeMathFont::instance().constants()
                             .overbarRuleThickness / 2.0);
    }
  }
}

void includeDelimiters(MathCssBox& root, const RootBoxContext& context) {
  const auto& layout = context.layout;
  const auto& semantic = context.semantic;
  const auto& scale = context.scale;
  const auto& cssRootFontPixelSize = context.cssRootFontPixelSize;
  root.height = std::max(root.height, rootLeftRightFenceExtent(
      layout.root.get(), semantic, root.height));
  if (root.semanticKind == MathSemanticKind::Fraction &&
      !middleDelimiterMarker(semantic) &&
      (!semantic->leftDelimiter.isEmpty() ||
       !semantic->rightDelimiter.isEmpty())) {
    const qreal fractionHeight = fractionMathMlHeight(
        semantic, cssRootFontPixelSize, scale);
    qreal delimiterTarget = fractionHeight > 0.0 ? fractionHeight : root.height;
    if (semantic->fractionHasBarLine) {
      delimiterTarget += semantic->fractionLineThicknessEm >= 0.0
          ? semantic->fractionLineThicknessEm * cssRootFontPixelSize *
                semantic->fractionSizeMultiplier
          : OpenTypeMathFont::instance().constants().fractionRuleThickness *
                semantic->fractionSizeMultiplier;
    }
    root.height = std::max(root.height, std::ceil(
        fractionDelimiterExtent(semantic, delimiterTarget)));
  }
}

void resolveBaselineAndInk(MathCssBox& root, const RootBoxContext& context) {
  const auto& semantic = context.semantic;
  root.baseline = root.height / 2.0 + kChromiumMathAxisOffsetPx;
  if (root.semanticKind == MathSemanticKind::Fraction)
    root.baseline = std::round(root.baseline * 2.0) / 2.0;
  if (root.semanticKind == MathSemanticKind::SupSub && semantic &&
      semantic->children.size() >= 3 &&
      hasAtomClass(semantic, QLatin1StringView("mop")))
    root.baseline = std::round(root.baseline * 2.0) / 2.0;
  if (root.semanticKind == MathSemanticKind::SupSub && semantic) {
    if (const MathRenderNode* accent = firstKind(
            semantic, MathRenderKind::Accent);
        accent && accent->children.size() >= 2) {
      const bool bodyIsLast = accent->accentKind == MathAccentKind::Under ||
                              accent->accentKind == MathAccentKind::OverBrace;
      const MathRenderNode* body = bodyIsLast
          ? accent->children.back().get() : accent->children.front().get();
      if (firstKind(body, MathRenderKind::Accent))
        root.baseline = std::ceil(root.baseline * 4.0) / 4.0;
    }
  }
  root.inkTop = 0.0;
  root.inkBottom = root.height;
  switch (root.semanticKind) {
    case MathSemanticKind::Fraction:
      root.inkTop = -4.0;
      root.inkBottom = root.height + 4.0;
      break;
    case MathSemanticKind::Radical:
      if (hasNestedSemantic(semantic, MathSemanticKind::SupSub)) {
        const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
        root.inkTop = constants.radicalVerticalGap + constants.radicalExtraAscender +
                      constants.radicalRuleThickness / 2.0;
        root.inkBottom = root.baseline + 1.6;
      }
      break;
    case MathSemanticKind::Array:
      root.inkTop = OpenTypeMathFont::instance().constants().axisHeight -
                    OpenTypeMathFont::instance().constants().fractionRuleThickness / 4.0;
      root.inkBottom = root.height + 0.5;
      break;
    case MathSemanticKind::SupSub:
    case MathSemanticKind::None:
      break;
  }
}

}  // namespace

MathCssBox layoutMathMlCssBox(const MathLayoutResult& layout,
                              qreal renderFontPixelSize,
                              qreal cssRootFontPixelSize) {
  MathCssBox root;
  root.role = MathCssBoxRole::Root;
  if (!layout.valid() || renderFontPixelSize <= 0.0 || cssRootFontPixelSize <= 0.0)
    return root;

  const qreal scale = cssRootFontPixelSize / renderFontPixelSize;
  root.children.push_back(convertTree(*layout.root, scale));
  const MathRenderNode* semantic = primarySemanticNode(layout.root.get());
  root.semanticKind = semantic ? semantic->semanticKind : MathSemanticKind::None;
  root.scriptKind = semantic ? semantic->scriptKind : MathScriptKind::None;
  root.radicalIndex = semantic && semantic->radicalIndex;
  const qreal scaledWidth = layout.naturalSize.width() * scale;
  const qreal scaledHeight = layout.naturalSize.height() * scale;

  const RootBoxContext context{layout, semantic, scale, cssRootFontPixelSize, scaledWidth, scaledHeight};
  switch (root.semanticKind) {
    case MathSemanticKind::Fraction: measureFraction(root, context); break;
    case MathSemanticKind::Radical: measureRadical(root, context); break;
    case MathSemanticKind::SupSub: measureSupSub(root, context); break;
    case MathSemanticKind::Array: measureArray(root, context); break;
    case MathSemanticKind::None: measureNone(root, context); break;
  }
  includeEnclosingAccent(root, context);
  includeSiblingContent(root, context);
  includeDelimiters(root, context);
  resolveBaselineAndInk(root, context);
  return root;
}

std::optional<MathCssAccentBox> layoutMathMlAccentBox(
    const MathLayoutResult& layout, qreal renderFontPixelSize,
    qreal cssRootFontPixelSize) {
  if (!layout.valid() || renderFontPixelSize <= 0.0 || cssRootFontPixelSize <= 0.0)
    return std::nullopt;
  const MathRenderNode* accent =
      firstKind(layout.root.get(), MathRenderKind::Accent);
  if (!accent || accent->children.size() < 2)
    return std::nullopt;

  const bool brace = accent->accentKind == MathAccentKind::UnderBrace ||
                     accent->accentKind == MathAccentKind::OverBrace;
  const bool arrow = (accent->accentKind == MathAccentKind::Under ||
                      accent->accentKind == MathAccentKind::Over) &&
                     !accent->accentCharacter.isEmpty();
  const QString basicCharacter = basicAccentCharacter(accent);
  const bool basic = !basicCharacter.isEmpty();
  if (!brace && !arrow && !basic) return std::nullopt;
  const bool over = accent->accentKind == MathAccentKind::OverBrace ||
                    accent->accentKind == MathAccentKind::Over ||
                    accent->accentKind == MathAccentKind::Overline;
  const bool bodyIsLast = accent->accentKind == MathAccentKind::Under ||
                          accent->accentKind == MathAccentKind::OverBrace;
  const MathRenderNode* body = bodyIsLast ? accent->children.back().get()
                                         : accent->children.front().get();
  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  const qreal bodyHeight = accentBodyCssHeight(
      body, cssRootFontPixelSize / renderFontPixelSize);
  const qreal gap = over ? constants.overbarVerticalGap
                         : constants.underbarVerticalGap;
  const qreal rule = over ? constants.overbarRuleThickness
                          : constants.underbarRuleThickness;
  qreal accentHeight = !over && containsTextModeRun(body)
      ? std::ceil(2.0 * rule)
      : std::round(gap + 2.0 * rule);
  accentHeight -= embeddedBraceStyleReduction(body);
  const qreal extra = over ? constants.overbarExtraAscender
                           : constants.underbarExtraDescender;
  const MathCssBox root = layoutMathMlCssBox(
      layout, renderFontPixelSize, cssRootFontPixelSize);
  const qreal rootHeight = snapEighth(root.height);

  if (basic) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const auto glyph = font.glyph(basicCharacter);
    if (!glyph || glyph->glyphIndex == 0) return std::nullopt;
    const QRectF ink = font.rasterGlyphBounds(glyph->glyphIndex);
    if (ink.isEmpty()) return std::nullopt;
    const qreal glyphHeight = std::max<qreal>(
        1.0, std::round(rootHeight - bodyHeight - extra));
    const qreal bodyWidth = cssNodeWidth(
        body, cssRootFontPixelSize / renderFontPixelSize);
    MathCssAccentBox result;
    result.over = over;
    result.character = basicCharacter;
    result.body = QRectF((root.width - bodyWidth) / 2.0,
                         over ? rootHeight - bodyHeight : 0.0,
                         bodyWidth, bodyHeight);
    result.accent = QRectF((root.width - glyph->advance) / 2.0,
                           over ? result.body.top() - glyphHeight
                                : result.body.bottom(),
                           glyph->advance, glyphHeight);
    return result;
  }

  if (arrow) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    if (accent->accentUsesNaturalWidth) {
      const auto variant = font.horizontalVariant(
          accent->accentCharacter, 0.0);
      const auto base = font.glyph(accent->accentCharacter);
      if (!variant && !base) return std::nullopt;
      const qreal glyphWidth = variant ? variant->extent : base->advance;
      const qreal glyphHeight = variant
          ? variant->advance : std::ceil(base->inkBounds.height());
      MathCssAccentBox result;
      result.over = over;
      result.character = accent->accentCharacter;
      result.body = QRectF(0.0, rootHeight - bodyHeight,
                           root.width, bodyHeight);
      result.accent = QRectF(
          (root.width - glyphWidth) / 2.0,
          result.body.top() - glyphHeight,
          glyphWidth, glyphHeight);
      return result;
    }
    const HorizontalAccentSelection selection = selectHorizontalAccent(
        accent->accentCharacter, root.width);
    if (!selection.fixed && !selection.assembly && !selection.text)
      return std::nullopt;
    const qreal arrowHeight = selection.height();
    const qreal accentWidth = selection.text
        ? selection.text->advance
        : selection.fixed ? selection.fixed->extent
        : selection.assembly ? selection.assembly->extent : root.width;
    const qreal bodyWidth = cssNodeWidth(
        body, cssRootFontPixelSize / renderFontPixelSize);
    MathCssAccentBox result;
    result.over = over;
    result.character = accent->accentCharacter;
    result.body = QRectF((root.width - bodyWidth) / 2.0,
                         over ? rootHeight - bodyHeight : 0.0,
                         bodyWidth, bodyHeight);
    result.accent = QRectF((root.width - accentWidth) / 2.0,
                           over ? result.body.top() - arrowHeight
                                : result.body.bottom(),
                           accentWidth, arrowHeight);
    return result;
  }

  const qreal annotationHeight = std::max<qreal>(
      0.0, rootHeight - bodyHeight - accentHeight - 2.0 * gap - extra);

  MathCssAccentBox result;
  result.over = over;
  result.character = QString(QChar(over ? 0x23DE : 0x23DF));
  result.fontScale = constants.scriptPercentScaleDown;
  const qreal bodyWidth = cssNodeWidth(
      body, cssRootFontPixelSize / renderFontPixelSize);
  const qreal bodyLeft = (root.width - bodyWidth) / 2.0;
  if (over) {
    result.annotation = QRectF(0.0, 0.0, root.width, annotationHeight);
    result.body = QRectF(bodyLeft, rootHeight - bodyHeight,
                         bodyWidth, bodyHeight);
    result.accent = QRectF(bodyLeft,
                           result.body.top() - gap - accentHeight,
                           bodyWidth, accentHeight);
  } else {
    result.body = QRectF(bodyLeft, 0.0, bodyWidth, bodyHeight);
    result.accent = QRectF(bodyLeft, result.body.bottom() + gap,
                           bodyWidth, accentHeight);
    result.annotation = QRectF(0.0, rootHeight - annotationHeight,
                               root.width, annotationHeight);
  }
  return result;
}

}  // namespace muffin::math
