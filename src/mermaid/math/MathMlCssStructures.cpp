#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math::mathml {

MathMlStyleContext fractionRowStyle(const MathRenderNode* fraction, MathMlStyleContext parentStyle) {
  if (!fraction)
    return parentStyle.scriptStyle();
  const int fractionLevel = std::clamp(fraction->fractionStyleSize, 0, 2);
  return {std::max(parentStyle.scriptLevel, fractionLevel)};
}

std::optional<MathCssScriptOperation> buildScriptOperation(const MathRenderNode* script,
                                                           const MathRenderNode* containingNode, QRectF containingRect,
                                                           qreal renderScale) {
  if (!script || script->semanticKind != MathSemanticKind::SupSub)
    return std::nullopt;
  if (script->operatorKind == MathOperatorKind::Limits && script->children.size() >= 3) {
    const MathRenderNode* subscript = script->children.front().get();
    const MathRenderNode* base = script->children[1].get();
    const MathRenderNode* superscript = script->children.back().get();
    const MathRenderNode* operatorSymbol = singleSymbol(base);
    if (!operatorSymbol)
      return std::nullopt;

    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const MathFontConstants& constants = font.constants();
    const auto variant = font.verticalVariant(operatorSymbol->text, constants.displayOperatorMinHeight);
    if (!variant)
      return std::nullopt;

    const bool textModeLimits = containsTextModeRun(script);
    const CssTokenLineMetrics supMetrics =
        cssTokenLineMetrics(superscript, renderScale, constants.scriptPercentScaleDown);
    const CssTokenLineMetrics subMetrics =
        cssTokenLineMetrics(subscript, renderScale, constants.scriptPercentScaleDown);
    GlyphInkExtents supInk =
        textModeLimits ? supMetrics.ink : glyphInkExtents(superscript, constants.scriptPercentScaleDown);
    GlyphInkExtents subInk =
        textModeLimits ? subMetrics.ink : glyphInkExtents(subscript, constants.scriptPercentScaleDown);
    supInk.top = std::max(supInk.top, std::round(2.0 * constants.axisHeight * constants.scriptPercentScaleDown));
    subInk.top = std::max(subInk.top, std::round(2.0 * constants.axisHeight));
    const qreal supHeight = textModeLimits ? supMetrics.height() : supInk.top + supInk.bottom;
    const qreal subHeight = textModeLimits ? subMetrics.height() : subInk.top + subInk.bottom;
    const qreal operatorHeight = largeOperatorAllocationHeight(operatorSymbol->text, *variant);
    const qreal upperGap =
        textModeLimits ? std::max(constants.upperLimitGapMin, constants.upperLimitBaselineRiseMin - supInk.bottom)
                       : std::max(constants.upperLimitGapMin, constants.upperLimitBaselineRiseMin);
    const qreal lowerGap = std::max(constants.lowerLimitGapMin, constants.lowerLimitBaselineDropMin - subInk.top);
    const qreal height = supHeight + upperGap + operatorHeight + lowerGap + subHeight;
    const qreal baseWidth = variant->advance;
    const qreal supWidth = cssNodeWidth(superscript, renderScale);
    const qreal subWidth = cssNodeWidth(subscript, renderScale);
    const qreal width = std::max({baseWidth, supWidth, subWidth});
    const qreal operatorSpacing = 3.0 / 18.0 * font.pixelSize();
    const qreal left =
        containingRect.left() + cssNodeOffset(containingNode, script, renderScale).value_or(0.0) + operatorSpacing;
    const bool fillsContainingRow = primarySemanticNode(containingNode) == script;
    const qreal top =
        fillsContainingRow ? containingRect.top() : containingRect.top() + (containingRect.height() - height) / 2.0;
    MathCssScriptOperation result;
    result.kind = script->scriptKind;
    result.limits = true;
    result.lineAscent = supHeight + upperGap + operatorHeight / 2.0 + constants.axisHeight;
    result.lineDescent = height - result.lineAscent;
    result.container = QRectF(left, top, width, height);
    result.baseNode = base;
    result.superscriptNode = superscript;
    result.subscriptNode = subscript;
    result.base = QRectF(left + (width - baseWidth) / 2.0, top + supHeight + upperGap, baseWidth, operatorHeight);
    result.superscript = QRectF(left + (width - supWidth) / 2.0, top, supWidth, supHeight);
    result.subscript = QRectF(left + (width - subWidth) / 2.0, result.base.bottom() + lowerGap, subWidth, subHeight);
    result.container = snapVerticalLayoutRect(result.container);
    result.base = snapVerticalLayoutRect(result.base);
    result.superscript = snapVerticalLayoutRect(result.superscript);
    result.subscript = snapVerticalLayoutRect(result.subscript);
    result.lineAscent = result.base.center().y() - result.container.top() + constants.axisHeight;
    result.lineDescent = result.container.height() - result.lineAscent;
    result.largeOperatorGlyph = buildVerticalGlyphOperation(operatorSymbol->text, result.base, false);
    return result;
  }
  if (script->children.size() != 2)
    return std::nullopt;
  const MathRenderNode* base = script->children.front().get();
  const MathRenderNode* scriptBox = script->children.back().get();
  const MathRenderNode* superscript = nullptr;
  const MathRenderNode* subscript = nullptr;
  if (script->scriptKind == MathScriptKind::SubSup && scriptBox && scriptBox->children.size() >= 2) {
    subscript = scriptBox->children.front().get();
    superscript = scriptBox->children.back().get();
  } else if (script->scriptKind == MathScriptKind::Subscript) {
    subscript = scriptBox;
  } else {
    superscript = scriptBox;
  }

  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  const MathRenderNode* operatorSymbol = singleSymbol(base);
  const bool largeOperator =
      operatorSymbol && operatorSymbol->atomClass == QLatin1String("mop") &&
      OpenTypeMathFont::instance().verticalVariant(operatorSymbol->text, constants.displayOperatorMinHeight);
  const CssTokenLineMetrics baseMetrics = cssTokenLineMetrics(base, renderScale, 1.0);
  const CssTokenLineMetrics supMetrics =
      cssTokenLineMetrics(superscript, renderScale, constants.scriptPercentScaleDown);
  const CssTokenLineMetrics subMetrics = cssTokenLineMetrics(subscript, renderScale, constants.scriptPercentScaleDown);
  GlyphInkExtents baseInk = baseMetrics.ink;
  if (largeOperator)
    baseInk = {baseMetrics.ascent, baseMetrics.descent};
  const GlyphInkExtents& supInk = supMetrics.ink;
  const GlyphInkExtents& subInk = subMetrics.ink;
  qreal supShift = std::max({constants.superscriptShiftUpCramped, constants.superscriptBottomMin + supInk.bottom,
                             baseInk.top - constants.superscriptBaselineDropMax});
  qreal subShift = std::max({constants.subscriptShiftDown, subInk.top - constants.subscriptTopMax,
                             constants.subscriptBaselineDropMin + baseInk.bottom});
  if (script->scriptKind == MathScriptKind::SubSup) {
    qreal gap = supShift + subShift - supInk.bottom - subInk.top;
    const qreal missingGap = constants.subSuperscriptGapMin - gap;
    if (missingGap > 0.0) {
      const qreal superscriptRoom = constants.superscriptBottomMaxWithSubscript - (supShift - supInk.bottom);
      const qreal raise = std::max<qreal>(0.0, std::min(superscriptRoom, missingGap));
      supShift += raise;
      gap += raise;
      subShift += std::max<qreal>(0.0, constants.subSuperscriptGapMin - gap);
    }
  }
  const qreal lineAscent =
      script->scriptKind == MathScriptKind::Subscript ? baseInk.top : std::max(baseInk.top, supShift + supInk.top);
  const qreal lineDescent = script->scriptKind == MathScriptKind::Superscript
                                ? baseInk.bottom
                                : std::max(baseInk.bottom, subShift + subInk.bottom);
  const qreal height = lineAscent + lineDescent;
  qreal baseWidth = cssNodeWidth(base, renderScale);
  if (largeOperator) {
    if (const auto variant =
            OpenTypeMathFont::instance().verticalVariant(operatorSymbol->text, constants.displayOperatorMinHeight))
      baseWidth = variant->advance;
  }
  const qreal supOffset = 0.0;
  qreal subOffset = 0.0;
  if (largeOperator && subscript) {
    if (const auto variant =
            OpenTypeMathFont::instance().verticalVariant(operatorSymbol->text, constants.displayOperatorMinHeight))
      subOffset = -variant->italicCorrection;
  }
  qreal width = cssNodeWidth(script, renderScale);
  if (largeOperator) {
    width = baseWidth + std::max(cssNodeWidth(superscript, renderScale), cssNodeWidth(subscript, renderScale)) +
            constants.spaceAfterScript;
  }
  qreal left = containingRect.left() + cssNodeOffset(containingNode, script, renderScale).value_or(0.0);
  if (largeOperator)
    left += 3.0 / 18.0 * OpenTypeMathFont::instance().pixelSize();
  if (const MathRenderNode* leftRight = enclosingKind(containingNode, script, MathRenderKind::LeftRight))
    left = containingRect.left() + arrayDelimiterWidth(leftRight->leftDelimiter, containingRect.height());
  const bool fillsContainingRow = primarySemanticNode(containingNode) == script;
  const qreal top =
      fillsContainingRow ? containingRect.top() : containingRect.top() + (containingRect.height() - height) / 2.0;
  MathCssScriptOperation result;
  result.kind = script->scriptKind;
  result.lineAscent = lineAscent;
  result.lineDescent = lineDescent;
  result.container = fillsContainingRow ? QRectF(left, containingRect.top(), width, containingRect.height())
                                        : QRectF(left, top, width, height);
  result.baseNode = base;
  result.superscriptNode = superscript;
  result.subscriptNode = subscript;
  result.base = QRectF(left, top + lineAscent - baseMetrics.ascent, baseWidth, baseMetrics.height());
  if (superscript) {
    result.superscript = QRectF(left + baseWidth + supOffset, top + lineAscent - supShift - supInk.top,
                                cssNodeWidth(superscript, renderScale), supMetrics.height());
    if (middleDelimiterMarker(superscript))
      result.superscript.translate(0.0, snapEighth(constants.fractionRuleThickness * constants.scriptPercentScaleDown));
  }
  if (subscript) {
    result.subscript = QRectF(left + baseWidth + subOffset, top + lineAscent + subShift - subInk.top,
                              cssNodeWidth(subscript, renderScale), subMetrics.height());
  }
  if (largeOperator) {
    if (superscript) {
      result.superscript.setY(result.container.top());
      result.superscript.setHeight(std::ceil(supMetrics.height()));
    }
    if (subscript) {
      qreal subHeight = std::ceil(subMetrics.height());
      if (const MathRenderNode* symbol = singleSymbol(subscript);
          symbol && symbol->text.size() == 1 && symbol->text.front().isLetter()) {
        subHeight = std::max(subHeight, std::ceil(subMetrics.ink.top + subMetrics.ink.bottom));
      }
      result.subscript.setY(result.container.bottom() - subHeight);
      result.subscript.setHeight(subHeight);
    }
  }
  result.container = snapVerticalLayoutRect(result.container);
  result.base = snapVerticalLayoutRect(result.base);
  result.superscript = snapVerticalLayoutRect(result.superscript);
  result.subscript = snapVerticalLayoutRect(result.subscript);
  if (largeOperator) {
    result.largeOperatorGlyph = buildVerticalGlyphOperation(operatorSymbol->text, result.base, false);
  }
  return result;
}

std::optional<MathCssRadicalOperation> buildRadicalOperation(const MathRenderNode* radical,
                                                             const MathRenderNode* containingNode,
                                                             QRectF containingRect, qreal renderScale, bool topLevel) {
  if (!radical || radical->semanticKind != MathSemanticKind::Radical)
    return std::nullopt;
  const MathRenderNode* radicalBox = radical->radicalIndex ? indexedRadicalBox(radical) : radical;
  const MathRenderNode* body = radicalBody(radicalBox);
  if (!body)
    return std::nullopt;

  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const MathFontConstants& constants = font.constants();
  const qreal styleScale = mathStyleScale(radical);
  const qreal radicalGap = constants.radicalVerticalGap * styleScale;
  const qreal radicalRule = constants.radicalRuleThickness * styleScale;
  const qreal radicalExtra = constants.radicalExtraAscender * styleScale;
  const GlyphInkExtents bodyInk = glyphInkExtents(body, 1.0);
  const qreal bodyWidth = cssNodeWidth(body, renderScale);
  qreal bodyHeight =
      primarySemanticNode(body) ? cssNodeHeight(body, renderScale) : std::ceil(bodyInk.top + bodyInk.bottom);
  if (const auto glyph = nativeGlyphBox(singleSymbol(body)))
    bodyHeight = std::round(glyph->height);
  else if (!primarySemanticNode(body) && symbolCount(body) > 1 && !middleDelimiterMarker(body))
    bodyHeight += 1.0;
  qreal target = std::max(bodyInk.top + bodyInk.bottom, bodyHeight) + radicalGap + radicalRule;
  if (middleDelimiterMarker(body))
    target += kChromiumMathAxisOffsetPx;
  const MathRenderNode* enclosingLeftRight =
      topLevel ? enclosingKind(containingNode, radical, MathRenderKind::LeftRight) : nullptr;
  if (topLevel && !enclosingLeftRight && !radical->radicalIndex)
    target = std::max(target, containingRect.height() - radicalExtra);
  const auto variant = font.verticalVariant(QString(QChar(0x221A)), target / styleScale);
  if (!variant)
    return std::nullopt;

  const qreal bodyAscent = std::min(bodyHeight, bodyInk.top);
  const qreal lineAscent = std::max(bodyAscent, bodyInk.top + radicalGap + radicalRule + radicalExtra);
  const qreal lineDescent = std::max(bodyHeight - bodyAscent, variant->extent * styleScale + radicalExtra - lineAscent);
  const qreal height = lineAscent + lineDescent;
  qreal left = containingRect.left() + cssNodeOffset(containingNode, radical, renderScale).value_or(0.0);
  if (enclosingLeftRight)
    left = containingRect.left() + arrayDelimiterWidth(enclosingLeftRight->leftDelimiter, containingRect.height());
  const qreal degreeBaseLeft = left;
  const MathRenderNode* degreeNode = radical->radicalIndex ? indexedRadicalDegree(radical, radicalBox) : nullptr;
  const qreal degreeAdvance = degreeNode ? std::max<qreal>(0.0, constants.radicalKernBeforeDegree +
                                                                    radicalDegreeAdvanceWidth(degreeNode, renderScale) +
                                                                    constants.radicalKernAfterDegree)
                                         : 0.0;
  left += degreeNode ? constants.radicalKernBeforeDegree + degreeAdvance : 0.0;
  const bool fillsContainingRow = primarySemanticNode(containingNode) == radical;
  const qreal top =
      fillsContainingRow ? containingRect.top() : containingRect.top() + (containingRect.height() - height) / 2.0;
  const qreal intrinsicRadicalHeight = variant->extent * styleScale + radicalExtra;
  const qreal indexedExpansion = radical->radicalIndex && fillsContainingRow
                                     ? std::max<qreal>(0.0, containingRect.height() - intrinsicRadicalHeight)
                                     : 0.0;
  const qreal decorationShift = indexedExpansion > 0.0 ? indexedExpansion / 2.0 + radicalRule / 2.0 : 0.0;
  const qreal indexedBodyTop = top + radicalExtra + radicalRule + 2.0 * radicalGap +
                               (indexedExpansion > 0.0 ? indexedExpansion / 2.0 + radicalRule / 4.0 : 0.0);

  MathCssRadicalOperation result;
  result.lineAscent = lineAscent;
  result.lineDescent = lineDescent;
  result.container = fillsContainingRow ? QRectF(degreeBaseLeft, containingRect.top(),
                                                 radicalCssWidth(radical, renderScale), containingRect.height())
                                        : QRectF(degreeBaseLeft, top, radicalCssWidth(radical, renderScale), height);
  result.body = QRectF(left + variant->advance * styleScale,
                       radical->radicalIndex && fillsContainingRow ? indexedBodyTop : top + lineAscent - bodyAscent,
                       bodyWidth, bodyHeight);
  result.radicalRule.target = QRectF(result.body.left(), top + radicalExtra + decorationShift, bodyWidth, radicalRule);
  result.radicalGlyph.character = QString(QChar(0x221A));
  result.radicalGlyph.glyphIndex = variant->glyphIndex;
  result.radicalGlyph.target =
      QRectF(left, top + radicalExtra + decorationShift, variant->advance * styleScale, variant->extent * styleScale);
  result.bodyNode = body;
  result.degreeNode = degreeNode;
  if (result.degreeNode) {
    const qreal degreeScale = constants.scriptScriptPercentScaleDown;
    const qreal degreeWidth = primarySemanticNode(result.degreeNode)
                                  ? radicalDegreePaintWidth(result.degreeNode, renderScale)
                                  : cssNodeWidth(result.degreeNode, renderScale);
    const MathRenderNode* degreeSymbol =
        primarySemanticNode(result.degreeNode) ? nullptr : singleSymbol(result.degreeNode);
    const auto degreeGlyphBox = nativeGlyphBox(degreeSymbol);
    const qreal degreeHeight = degreeGlyphBox ? std::round(degreeGlyphBox->height * degreeScale)
                                              : radicalDegreePaintHeight(result.degreeNode, renderScale);
    const qreal degreeBottom =
        result.radicalGlyph.target.bottom() -
        constants.radicalDegreeBottomRaisePercent * result.radicalGlyph.target.height() +
        (fillsContainingRow ? std::max<qreal>(0.0, containingRect.height() - height) - decorationShift : 0.0);
    result.degree = QRectF(degreeBaseLeft + constants.radicalKernBeforeDegree, degreeBottom - degreeHeight, degreeWidth,
                           degreeHeight);
  }
  if (enclosingLeftRight) {
    MathCssFencePair fences;
    const qreal leftWidth = arrayDelimiterWidth(enclosingLeftRight->leftDelimiter, containingRect.height());
    const qreal rightWidth = arrayDelimiterWidth(enclosingLeftRight->rightDelimiter, containingRect.height());
    fences.left = QRectF(containingRect.left(), containingRect.top(), leftWidth, containingRect.height());
    fences.right =
        QRectF(containingRect.right() - rightWidth, containingRect.top(), rightWidth, containingRect.height());
    fences.leftCharacter = mathDelimiterCharacter(enclosingLeftRight->leftDelimiter);
    fences.rightCharacter = mathDelimiterCharacter(enclosingLeftRight->rightDelimiter);
    fences.leftGlyph = buildInlineFenceGlyphOperation(fences.leftCharacter, fences.left);
    fences.rightGlyph = buildInlineFenceGlyphOperation(fences.rightCharacter, fences.right);
    result.fences = std::move(fences);
    result.container = containingRect;
  }
  result.container = snapVerticalLayoutRect(result.container);
  result.body = snapVerticalLayoutRect(result.body);
  result.degree = snapVerticalLayoutRect(result.degree);
  result.radicalRule.target = snapVerticalLayoutRect(result.radicalRule.target);
  result.radicalGlyph.target = snapVerticalLayoutRect(result.radicalGlyph.target);
  result.radicalGlyph.fontScale = styleScale;
  result.radicalGlyph.inkBounds = font.rasterGlyphBounds(variant->glyphIndex, styleScale);
  result.radicalGlyph.clip = result.radicalGlyph.target.intersected(result.container);
  return result;
}

qreal nestedFractionCssHeight(const MathRenderNode* fraction, qreal cssRootFontPixelSize, qreal renderScale) {
  return fractionMathMlHeight(fraction, cssRootFontPixelSize, renderScale) +
         std::round(OpenTypeMathFont::instance().constants().fractionRuleThickness * fraction->fractionSizeMultiplier);
}

std::optional<MathCssPaintOperation> buildArrayOperation(const MathRenderNode* array,
                                                         const MathRenderNode* containingNode, QRectF containingRect,
                                                         qreal renderScale, qreal cssRootFontPixelSize,
                                                         MathMlStyleContext style) {
  if (!array || array->semanticKind != MathSemanticKind::Array || array->rows <= 0 || array->columns <= 0)
    return std::nullopt;
  const MathRenderNode* table = arrayTableBody(array, array->columns);
  if (!table)
    return std::nullopt;

  QString leftDelimiter = array->arrayLeftDelimiter;
  QString rightDelimiter = array->arrayRightDelimiter;
  if (const MathRenderNode* leftRight = enclosingKind(containingNode, array, MathRenderKind::LeftRight)) {
    if (leftDelimiter.isEmpty())
      leftDelimiter = leftRight->leftDelimiter;
    if (rightDelimiter.isEmpty())
      rightDelimiter = leftRight->rightDelimiter;
  }

  const QVector<qreal> rowHeights = arrayCssRowHeights(array, table, renderScale);
  qreal tableHeight = 0.0;
  for (qreal height : rowHeights)
    tableHeight += height;
  QVector<qreal> columnWidths(array->columns);
  qreal tableWidth = 0.0;
  const qreal horizontalPadding = OpenTypeMathFont::instance().pixelSize() * kMathTableCellHorizontalPaddingEm;
  for (int column = 0; column < array->columns; ++column) {
    const qreal contentWidth = column < static_cast<int>(table->children.size())
                                   ? cssNodeWidth(table->children[static_cast<size_t>(column)].get(), renderScale)
                                   : 0.0;
    columnWidths[column] = contentWidth + horizontalPadding;
    tableWidth += columnWidths[column];
  }

  const qreal leftWidth = arrayDelimiterWidth(leftDelimiter, tableHeight);
  const qreal rightWidth = arrayDelimiterWidth(rightDelimiter, tableHeight);
  const qreal intrinsicWidth = leftWidth + tableWidth + rightWidth;
  const bool wrappedByLeftRight = enclosingKind(containingNode, array, MathRenderKind::LeftRight);
  const bool fillsContainingRegion = primarySemanticNode(containingNode) == array &&
                                     (symbolCount(containingNode) == symbolCount(array) || wrappedByLeftRight);
  const qreal left = fillsContainingRegion
                         ? containingRect.left()
                         : containingRect.left() + cssNodeOffset(containingNode, array, renderScale).value_or(0.0);
  qreal delimiterHeight = 0.0;
  for (const QString& delimiter : {leftDelimiter, rightDelimiter}) {
    if (delimiter.isEmpty() || delimiter == QLatin1String("."))
      continue;
    if (const auto variant =
            OpenTypeMathFont::instance().verticalVariant(mathDelimiterCharacter(delimiter), tableHeight))
      delimiterHeight = std::max(delimiterHeight, variant->extent);
  }
  const qreal height =
      fillsContainingRegion ? containingRect.height() : std::max(tableHeight, std::ceil(delimiterHeight));
  const qreal top =
      fillsContainingRegion ? containingRect.top() : containingRect.top() + (containingRect.height() - height) / 2.0;

  MathCssPaintOperation operation;
  operation.payload = MathCssArrayOperation{};
  MathCssArrayOperation& result = std::get<MathCssArrayOperation>(operation.payload);
  result.container = QRectF(left, top, fillsContainingRegion ? containingRect.width() : intrinsicWidth, height);
  result.table = QRectF(left + leftWidth, top + (height - tableHeight) / 2.0, tableWidth, tableHeight);
  result.leftDelimiter = QRectF(left, top, leftWidth, height);
  result.rightDelimiter = QRectF(result.table.right(), top, rightWidth, height);
  result.leftDelimiterCharacter = mathDelimiterCharacter(leftDelimiter);
  result.rightDelimiterCharacter = mathDelimiterCharacter(rightDelimiter);
  result.leftDelimiterGlyph = buildVerticalGlyphOperation(result.leftDelimiterCharacter, result.leftDelimiter);
  result.rightDelimiterGlyph = buildVerticalGlyphOperation(result.rightDelimiterCharacter, result.rightDelimiter);
  result.lineAscent = result.container.height() / 2.0 + kChromiumMathAxisOffsetPx;

  struct CellMetrics {
    const MathRenderNode* node = nullptr;
    qreal width = 0.0;
    qreal height = 0.0;
    qreal baseline = 0.0;
  };
  QVector<CellMetrics> cells(array->rows * array->columns);
  const auto cellIndex = [array](int row, int column) { return row * array->columns + column; };
  for (int row = 0; row < array->rows; ++row) {
    for (int column = 0; column < array->columns; ++column) {
      CellMetrics& cell = cells[cellIndex(row, column)];
      cell.node = arrayCellNode(array, table, row, column);
      cell.width = cssNodeWidth(cell.node, renderScale);
      cell.height = cssNodeHeight(cell.node, renderScale);
      const MathRenderNode* semantic = primarySemanticNode(cell.node);
      if (semantic && semantic->semanticKind == MathSemanticKind::Radical &&
          hasNestedSemantic(semantic, MathSemanticKind::SupSub)) {
        const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
        cell.height -= constants.radicalExtraAscender - constants.radicalRuleThickness / 2.0;
      }
      cell.height -= embeddedBraceStyleReduction(cell.node);
      if (!semantic && symbolCount(cell.node) > 1) {
        const GlyphInkExtents ink = glyphInkExtents(cell.node, 1.0);
        cell.height = std::ceil(ink.top + ink.bottom) +
                      (hasMathMlOperatorOverflow(cell.node)
                           ? std::floor(OpenTypeMathFont::instance().constants().fractionRuleThickness)
                           : 0.0);
        if (hasAtomClass(cell.node, QLatin1StringView("mrel")))
          cell.height = std::max<qreal>(
              cell.height, cssRootFontPixelSize * 0.75 +
                               (hasMathMlOperatorOverflow(cell.node)
                                    ? std::floor(OpenTypeMathFont::instance().constants().fractionRuleThickness)
                                    : 0.0));
      }
      if (semantic && semantic->semanticKind == MathSemanticKind::Fraction && semantic->fractionStyleSize > 0) {
        cell.height = nestedFractionCssHeight(semantic, cssRootFontPixelSize, renderScale);
        cell.height = std::max(cell.height, std::ceil(rootLeftRightFenceExtent(cell.node, semantic, cell.height)));
      }
      if (semantic && ownsGenericPaintOperation(semantic)) {
        const QRectF probeRect(0.0, 0.0, cell.width, cell.height);
        if (const auto probe =
                buildPaintOperation(semantic, cell.node, probeRect, renderScale, cssRootFontPixelSize, style)) {
          cell.baseline =
              semantic->semanticKind == MathSemanticKind::SupSub && semantic->scriptKind == MathScriptKind::Superscript
                  ? semantic->height
              : semantic->semanticKind == MathSemanticKind::SupSub && semantic->scriptKind == MathScriptKind::Subscript
                  ? std::round(semantic->height)
              : semantic->semanticKind == MathSemanticKind::SupSub ||
                      (semantic->semanticKind == MathSemanticKind::Radical &&
                       !hasNestedSemantic(semantic, MathSemanticKind::SupSub))
                  ? probe->lineAscent()
              : semantic->semanticKind == MathSemanticKind::Radical
                  ? probe->alignmentBaseline() - OpenTypeMathFont::instance().constants().radicalExtraAscender
                  : probe->alignmentBaseline();
        }
      }
      if (semantic && semantic->semanticKind == MathSemanticKind::Fraction &&
          firstKind(cell.node, MathRenderKind::LeftRight)) {
        cell.baseline = cell.height / 2.0 + OpenTypeMathFont::instance().constants().axisHeight;
      }
      if (cell.baseline <= 0.0) {
        const GlyphInkExtents ink = glyphInkExtents(cell.node, 1.0);
        cell.baseline = std::min(cell.height, ink.top);
      }
      if (!semantic && array->arrayEnvironment == QLatin1String("cases")) {
        cell.baseline = cell.node->height + 2.0 * cell.node->depth;
      }
    }
  }

  const qreal verticalPadding = OpenTypeMathFont::instance().pixelSize() * kMathTableCellVerticalPaddingEm / 2.0;
  qreal rowTop = result.table.top();
  for (int row = 0; row < array->rows; ++row) {
    const QRectF rowRect(result.table.left(), rowTop, result.table.width(), rowHeights[row]);
    result.rows.push_back(rowRect);
    qreal maximumAscent = 0.0;
    for (int column = 0; column < array->columns; ++column)
      maximumAscent = std::max(maximumAscent, cells[cellIndex(row, column)].baseline);

    qreal columnLeft = result.table.left();
    for (int column = 0; column < array->columns; ++column) {
      const CellMetrics& measured = cells[cellIndex(row, column)];
      MathCssArrayCell cell;
      cell.row = row;
      cell.column = column;
      cell.box = QRectF(columnLeft, rowTop, columnWidths[column], rowHeights[row]);
      cell.content =
          QRectF(columnLeft + (columnWidths[column] - measured.width) / 2.0,
                 rowTop + verticalPadding + maximumAscent - measured.baseline, measured.width, measured.height);
      cell.baseline = measured.baseline;
      cell.contentNode = measured.node;
      if (auto runs = buildGlyphRunOperations(cell.contentNode, cell.content, style.fontScale(), true))
        cell.glyphRuns = std::move(*runs);
      result.cells.push_back(cell);
      columnLeft += columnWidths[column];
    }
    rowTop += rowHeights[row];
  }

  for (const MathCssArrayCell& cell : result.cells) {
    QVector<const MathRenderNode*> nodes;
    collectImmediatePaintNodes(cell.contentNode, &nodes);
    for (const MathRenderNode* node : nodes) {
      if (auto child =
              buildPaintOperation(node, cell.contentNode, cell.content, renderScale, cssRootFontPixelSize, style))
        operation.children.push_back(std::move(*child));
    }
  }
  return operation;
}

std::optional<MathCssPaintOperation> buildFractionOperation(const MathRenderNode* fraction,
                                                            const MathRenderNode* containingNode, qreal fractionOffset,
                                                            qreal semanticTop, qreal semanticHeight, qreal renderScale,
                                                            qreal cssRootFontPixelSize, bool nested,
                                                            MathMlStyleContext style) {
  const auto metrics = fractionMetrics(fraction, cssRootFontPixelSize, renderScale);
  if (!metrics)
    return std::nullopt;
  const MathRenderNode* stack = directChild(fraction, MathRenderKind::VList);
  if (!stack)
    return std::nullopt;
  const MathMlStyleContext rowStyle = fractionRowStyle(fraction, style);
  const qreal contentWidth = cssNodeWidth(stack, renderScale) + 2.0;
  const qreal targetHeight = (fraction->height + fraction->depth) * renderScale;
  QString leftDelimiter = fraction->leftDelimiter;
  QString rightDelimiter = fraction->rightDelimiter;
  bool inheritedLeftDelimiter = false;
  if (const MathRenderNode* leftRight = enclosingKind(containingNode, fraction, MathRenderKind::LeftRight)) {
    if (leftDelimiter.isEmpty()) {
      leftDelimiter = leftRight->leftDelimiter;
      inheritedLeftDelimiter = true;
    }
    if (rightDelimiter.isEmpty())
      rightDelimiter = leftRight->rightDelimiter;
  }
  const qreal leftWidth = arrayDelimiterWidth(leftDelimiter, targetHeight);
  const qreal rightWidth = arrayDelimiterWidth(rightDelimiter, targetHeight);
  if (inheritedLeftDelimiter)
    fractionOffset -= leftWidth;
  const qreal fractionLeft = fractionOffset + leftWidth;
  qreal fractionHeight = metrics->extents.top + metrics->extents.bottom;
  const qreal alignmentRule =
      metrics->ruleThickness > 0.0
          ? metrics->ruleThickness
          : OpenTypeMathFont::instance().constants().fractionRuleThickness * fraction->fractionSizeMultiplier;
  const bool hasDelimiter = leftWidth > 0.0 || rightWidth > 0.0;
  if (nested && !hasDelimiter) {
    fractionHeight +=
        std::round(OpenTypeMathFont::instance().constants().fractionRuleThickness * fraction->fractionSizeMultiplier);
  }
  if (!hasDelimiter)
    fractionHeight = std::max(fractionHeight, semanticHeight);
  const qreal fractionTop = hasDelimiter
                                ? middleDelimiterMarker(fraction)
                                      ? semanticTop + alignmentRule / 2.0
                                      : semanticTop + (semanticHeight - fractionHeight) / 2.0 - alignmentRule / 2.0
                                : semanticTop;
  const qreal numeratorWidth = cssNodeWidth(metrics->numerator, renderScale);
  const qreal denominatorWidth = cssNodeWidth(metrics->denominator, renderScale);
  qreal numeratorHeight = metrics->numeratorInk.top + metrics->numeratorInk.bottom;
  qreal denominatorHeight = metrics->denominatorInk.top + metrics->denominatorInk.bottom;
  const bool plainRows = !primarySemanticNode(metrics->numerator) && !primarySemanticNode(metrics->denominator);
  const int numeratorSymbols = symbolCount(metrics->numerator);
  const int denominatorSymbols = symbolCount(metrics->denominator);
  if (plainRows && numeratorSymbols > 1 && denominatorSymbols > 1) {
    // Native mfrac gives sibling token rows the same inline ink line box.
    const qreal sharedRowHeight = std::max(numeratorHeight, denominatorHeight);
    numeratorHeight = sharedRowHeight;
    denominatorHeight = sharedRowHeight;
  } else {
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    const qreal childStyleScale = fraction->fractionStyleSize == 0   ? 1.0
                                  : fraction->fractionStyleSize == 1 ? constants.scriptPercentScaleDown
                                                                     : constants.scriptScriptPercentScaleDown;
    if (const auto glyph = nativeGlyphBox(singleSymbol(metrics->numerator)))
      numeratorHeight = std::round(glyph->height * childStyleScale);
    else
      numeratorHeight = cssNodeHeight(metrics->numerator, renderScale);
    if (const auto glyph = nativeGlyphBox(singleSymbol(metrics->denominator)))
      denominatorHeight = std::round(glyph->height * childStyleScale);
    else
      denominatorHeight = cssNodeHeight(metrics->denominator, renderScale);
  }
  // Row paint boxes must contain the extents used to size the fraction.
  numeratorHeight = std::max(numeratorHeight, metrics->numeratorInk.top + metrics->numeratorInk.bottom);
  denominatorHeight = std::max(denominatorHeight, metrics->denominatorInk.top + metrics->denominatorInk.bottom);
  const auto includeImmediateOperationHeight = [&](const MathRenderNode* row, qreal height) {
    QVector<const MathRenderNode*> nodes;
    collectImmediatePaintNodes(row, &nodes);
    const QRectF intrinsicRow(0.0, 0.0, cssNodeWidth(row, renderScale), 0.0);
    for (const MathRenderNode* node : nodes) {
      const bool recursive = hasNestedSemantic(node, MathSemanticKind::Fraction) ||
                             hasNestedSemantic(node, MathSemanticKind::Radical) ||
                             hasNestedSemantic(node, MathSemanticKind::SupSub);
      if (recursive) {
        const qreal recursiveHeight = cssNodeHeight(node, renderScale);
        height = std::max(height, recursiveHeight);
        continue;
      }
      switch (node->semanticKind) {
      case MathSemanticKind::Fraction:
        height = std::max(height, nestedFractionCssHeight(node, cssRootFontPixelSize, renderScale));
        break;
      case MathSemanticKind::SupSub:
        if (const auto child = buildScriptOperation(node, row, intrinsicRow, renderScale)) {
          height = std::max({height, child->lineAscent + child->lineDescent, child->base.bottom(),
                             child->superscript.bottom(), child->subscript.bottom()});
        }
        break;
      case MathSemanticKind::Radical:
        if (const auto child = buildRadicalOperation(node, row, intrinsicRow, renderScale)) {
          height = std::max({height, child->lineAscent + child->lineDescent, child->radicalGlyph.target.bottom(),
                             child->radicalRule.target.bottom(), child->body.bottom()});
        }
        break;
      case MathSemanticKind::Array:
      case MathSemanticKind::None:
        break;
      }
    }
    return snapLayoutUnit(height);
  };
  numeratorHeight = includeImmediateOperationHeight(metrics->numerator, numeratorHeight);
  denominatorHeight = includeImmediateOperationHeight(metrics->denominator, denominatorHeight);
  MathCssPaintOperation operation;
  operation.payload = MathCssFractionPaint{};
  MathCssFractionPaint& fractionPaint = std::get<MathCssFractionPaint>(operation.payload);
  fractionPaint.numeratorNode = metrics->numerator;
  fractionPaint.denominatorNode = metrics->denominator;
  MathCssFractionBox& result = fractionPaint.box;
  result.hasRule = fraction->fractionHasBarLine;
  result.styleSize = fraction->fractionStyleSize;
  result.fontScale = fraction->fractionSizeMultiplier;
  result.container = QRectF(fractionOffset, semanticTop, leftWidth + contentWidth + rightWidth, semanticHeight);
  result.fraction = QRectF(fractionLeft, fractionTop, contentWidth, fractionHeight);
  result.numerator = QRectF(fractionLeft + 1.0 + (contentWidth - 2.0 - numeratorWidth) / 2.0, fractionTop,
                            numeratorWidth, numeratorHeight);
  result.denominator = QRectF(fractionLeft + 1.0 + (contentWidth - 2.0 - denominatorWidth) / 2.0,
                              fractionTop + fractionHeight - denominatorHeight, denominatorWidth, denominatorHeight);
  result.leftDelimiter = QRectF(fractionOffset, semanticTop, leftWidth, semanticHeight);
  result.rightDelimiter = QRectF(fractionLeft + contentWidth, semanticTop, rightWidth, semanticHeight);
  result.leftDelimiterCharacter = mathDelimiterCharacter(leftDelimiter);
  result.rightDelimiterCharacter = mathDelimiterCharacter(rightDelimiter);
  fractionPaint.leftDelimiterGlyph = buildVerticalGlyphOperation(result.leftDelimiterCharacter, result.leftDelimiter);
  fractionPaint.rightDelimiterGlyph =
      buildVerticalGlyphOperation(result.rightDelimiterCharacter, result.rightDelimiter);
  if (auto runs = buildGlyphRunOperations(metrics->numerator, result.numerator, rowStyle.fontScale(), true))
    fractionPaint.numeratorGlyphRuns = std::move(*runs);
  if (auto runs = buildGlyphRunOperations(metrics->denominator, result.denominator, rowStyle.fontScale(), true))
    fractionPaint.denominatorGlyphRuns = std::move(*runs);

  const auto appendChildren = [&](const MathRenderNode* row, QRectF rowRect) {
    QVector<const MathRenderNode*> nodes;
    collectImmediatePaintNodes(row, &nodes);
    for (const MathRenderNode* node : nodes) {
      if (auto child = buildPaintOperation(node, row, rowRect, renderScale, cssRootFontPixelSize, rowStyle))
        operation.children.push_back(std::move(*child));
    }
  };
  appendChildren(metrics->numerator, result.numerator);
  appendChildren(metrics->denominator, result.denominator);

  const auto rowLineAscent = [&](const MathRenderNode* row, QRectF rowRect) {
    for (const MathCssPaintOperation& child : operation.children) {
      if (child.container().intersects(rowRect))
        return child.container().top() - rowRect.top() + child.lineAscent();
    }
    const qreal operatorLeading = symbolCount(row) > 1 ? 1.0 : 0.0;
    return std::max<qreal>(0.0, rowRect.height() - operatorLeading);
  };
  const qreal numeratorAscent = rowLineAscent(metrics->numerator, result.numerator);
  const qreal denominatorAscent = rowLineAscent(metrics->denominator, result.denominator);
  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  if (result.hasRule) {
    fractionPaint.lineAscent =
        std::max({metrics->numeratorShift + numeratorAscent, -metrics->denominatorShift + denominatorAscent,
                  constants.axisHeight + metrics->ruleThickness / 2.0});
    const qreal ruleCenter = fractionTop + fractionPaint.lineAscent - constants.axisHeight;
    result.rule = QRectF(fractionLeft + 1.0, ruleCenter - metrics->ruleThickness / 2.0, contentWidth - 2.0,
                         metrics->ruleThickness);
  } else {
    fractionPaint.lineAscent = metrics->extents.top;
  }
  return operation;
}

} // namespace muffin::math::mathml
