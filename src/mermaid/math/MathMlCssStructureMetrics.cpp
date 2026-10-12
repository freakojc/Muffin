#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math::mathml {

qreal arrayDelimiterWidth(const QString& delimiter, qreal targetHeight) {
  if (delimiter.isEmpty() || delimiter == QLatin1String(".")) return 0.0;
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const QString character = mathDelimiterCharacter(delimiter);
  const qreal operatorPadding =
      character == QString(QChar(0x2223)) ||
              character == QString(QChar(0x2225))
          ? snapLayoutUnit(font.pixelSize() * kMathMlThickSpaceEm)
          : 0.0;
  const auto assembly = font.verticalAssembly(character, targetHeight);
  const auto fixed = font.verticalVariant(
      character, std::numeric_limits<qreal>::max());
  qreal width = 0.0;
  if (assembly && fixed) {
    if (character == QLatin1String("{") || character == QLatin1String("}"))
      width = std::max(assembly->advance, fixed->advance);
    else
      width = fixed->advance;
  } else if (assembly) {
    width = assembly->advance;
  } else if (fixed) {
    width = fixed->advance;
  } else if (const auto base = font.glyph(character)) {
    width = base->advance;
  }
  return width + 2.0 * operatorPadding;
}

const MathRenderNode* indexedRadicalBox(const MathRenderNode* node) {
  if (!node) return nullptr;
  const MathRenderNode* best = nullptr;
  for (const auto& child : node->children) {
    if (!child) continue;
    const MathRenderNode* candidate = child->semanticKind ==
            MathSemanticKind::Radical
        ? child.get()
        : indexedRadicalBox(child.get());
    if (candidate &&
        (!best || mathStyleScale(candidate) > mathStyleScale(best)))
      best = candidate;
  }
  return best;
}

qreal leftRightBodyCssWidth(const MathRenderNode* node, qreal scale,
                            bool root) {
  if (!node) return 0.0;
  if (!node->middleDelimiter.isEmpty()) return cssNodeWidth(node, scale);
  if (!root && node->kind == MathRenderKind::LeftRight)
    return cssNodeWidth(node, scale);
  if (node->semanticKind != MathSemanticKind::None)
    return cssNodeWidth(node, scale);
  if (node->children.empty()) return cssNodeWidth(node, scale);
  if (node->kind != MathRenderKind::Span)
    return cssNodeWidth(node, scale);
  qreal width = 0.0;
  for (const auto& child : node->children)
    width += leftRightBodyCssWidth(child.get(), scale, false);
  return width;
}

const MathRenderNode* arrayTableBody(const MathRenderNode* node, int columns) {
  if (node == nullptr) return nullptr;
  if (node->kind == MathRenderKind::VList &&
      static_cast<int>(node->children.size()) == columns && columns > 0)
    return node;
  for (const auto& child : node->children)
    if (const MathRenderNode* body = arrayTableBody(child.get(), columns)) return body;
  return nullptr;
}

qreal arrayCssWidth(const MathRenderNode* array, qreal scale) {
  qreal width = 0.0;
  const qreal cssFontSize = OpenTypeMathFont::instance().pixelSize();
  const MathRenderNode* table = arrayTableBody(array, array->columns);
  for (int column = 0; column < array->columns; ++column) {
    qreal columnWidth = column < static_cast<int>(array->arrayColumnWidths.size())
        ? array->arrayColumnWidths[static_cast<size_t>(column)] * scale : 0.0;
    if (table && column < static_cast<int>(table->children.size())) {
      columnWidth = cssNodeWidth(table->children[static_cast<size_t>(column)].get(), scale);
    } else {
      columnWidth = std::ceil(columnWidth);
    }
    width += columnWidth +
             cssFontSize * kMathTableCellHorizontalPaddingEm;
  }
  if (!table)
    width -= std::max(0, array->columns - 2) * kMathTableExtraColumnCorrectionPx;
  const qreal targetHeight = arrayCssHeight(array, scale);
  width += arrayDelimiterWidth(array->arrayLeftDelimiter, targetHeight);
  width += arrayDelimiterWidth(array->arrayRightDelimiter, targetHeight);
  return std::floor(width * 16.0) / 16.0;
}

const MathRenderNode* columnRows(const MathRenderNode* node, int rows) {
  if (node == nullptr) return nullptr;
  if (node->kind == MathRenderKind::VList &&
      static_cast<int>(node->children.size()) == rows && rows > 0)
    return node;
  for (const auto& child : node->children)
    if (const MathRenderNode* result = columnRows(child.get(), rows)) return result;
  return nullptr;
}

const MathRenderNode* arrayCellNode(const MathRenderNode* array,
                                    const MathRenderNode* table,
                                    int row, int column) {
  if (!array || !table || row < 0 || column < 0 ||
      column >= static_cast<int>(table->children.size()))
    return nullptr;
  const MathRenderNode* rows = columnRows(
      table->children[static_cast<size_t>(column)].get(), array->rows);
  if (!rows || row >= static_cast<int>(rows->children.size())) return nullptr;
  const MathRenderNode* cell = rows->children[static_cast<size_t>(row)].get();
  if ((array->arrayEnvironment == QLatin1String("cases") ||
       array->arrayEnvironment == QLatin1String("aligned")) &&
      cell && cell->children.size() == 1)
    cell = cell->children.front().get();
  return cell;
}

QVector<qreal> arrayCssRowHeights(const MathRenderNode* array,
                                  const MathRenderNode* table,
                                  qreal scale) {
  QVector<qreal> heights;
  if (!array || !table || array->rows <= 0) return heights;
  heights.resize(array->rows);
  const bool fixedLineRows = array->arrayEnvironment == QLatin1String("cases") ||
                             array->arrayEnvironment == QLatin1String("aligned");
  const qreal cssFontSize = OpenTypeMathFont::instance().pixelSize();
  for (int row = 0; row < array->rows; ++row) {
    qreal contentHeight = 0.0;
    bool plainDescender = false;
    bool textModeDescender = false;
    bool operatorOverflow = false;
    bool scriptOverflow = false;
    bool underAccentOverflow = false;
    bool structuredCell = false;
    for (int column = 0; column < array->columns; ++column) {
      const MathRenderNode* cell = arrayCellNode(array, table, row, column);
      structuredCell = structuredCell || primarySemanticNode(cell) ||
                       firstKind(cell, MathRenderKind::Accent);
      qreal currentHeight = cssNodeHeight(cell, scale);
      if (const MathRenderNode* semantic = primarySemanticNode(cell);
          semantic && semantic->semanticKind == MathSemanticKind::Fraction &&
          semantic->fractionStyleSize > 0) {
        currentHeight = nestedFractionCssHeight(
            semantic, cssFontSize, scale);
        currentHeight = std::max(currentHeight, std::ceil(
            rootLeftRightFenceExtent(cell, semantic, currentHeight)));
      }
      if (const MathRenderNode* semantic = primarySemanticNode(cell);
          semantic && semantic->semanticKind == MathSemanticKind::Radical &&
          hasNestedSemantic(semantic, MathSemanticKind::SupSub)) {
        const MathFontConstants& constants =
            OpenTypeMathFont::instance().constants();
        currentHeight -= constants.radicalExtraAscender -
                         constants.radicalRuleThickness / 2.0;
      }
      const qreal braceReduction = embeddedBraceStyleReduction(cell);
      currentHeight -= braceReduction;
      underAccentOverflow = underAccentOverflow ||
                            (braceReduction > 0.0 && hasUnderBrace(cell));
      contentHeight = std::max(contentHeight, currentHeight);
      plainDescender = plainDescender || hasPlainLatinDescender(cell);
      textModeDescender = textModeDescender || hasTextModeDescender(cell);
      operatorOverflow = operatorOverflow || hasMathMlOperatorOverflow(cell);
      scriptOverflow = scriptOverflow ||
          hasNestedSemantic(cell, MathSemanticKind::SupSub);
    }
    const bool singleRowTableLine = array->rows == 1 &&
                                    array->columns > 1 &&
                                    !structuredCell;
    if (fixedLineRows || singleRowTableLine) {
      const MathFontConstants& constants =
          OpenTypeMathFont::instance().constants();
      qreal rowHeight = cssFontSize * kMathTableRowHeightEm;
      if (operatorOverflow) rowHeight += constants.fractionRuleThickness;
      if (scriptOverflow)
        rowHeight += constants.spaceAfterScript +
                     constants.fractionRuleThickness;
      heights[row] = rowHeight;
    } else {
      heights[row] = contentHeight +
          cssFontSize *
              (kMathTableCellVerticalPaddingEm +
               ((plainDescender || underAccentOverflow)
                    ? kMathTableDescenderExpansionEm
                    : textModeDescender && array->columns > 1
                        ? kMathTableTextDescenderExpansionEm : 0.0));
    }
  }
  return heights;
}

qreal arrayCssHeight(const MathRenderNode* array, qreal scale) {
  const MathRenderNode* table = arrayTableBody(array, array->columns);
  if (!table) return 0.0;
  qreal height = 0.0;
  for (qreal rowHeight : arrayCssRowHeights(array, table, scale))
    height += rowHeight;
  return height;
}

qreal fractionCssWidth(const MathRenderNode* fraction, qreal scale) {
  const MathRenderNode* body = directChild(fraction, MathRenderKind::VList);
  const qreal contentWidth = body ? cssNodeWidth(body, scale)
                                  : widestDescendant(fraction, MathRenderKind::VList) * scale;
  qreal width = std::max<qreal>(0.0, contentWidth + 2.0);
  const qreal targetHeight = (fraction->height + fraction->depth) * scale;
  width += arrayDelimiterWidth(fraction->leftDelimiter, targetHeight);
  width += arrayDelimiterWidth(fraction->rightDelimiter, targetHeight);
  return width;
}

qreal radicalDegreePaintWidth(const MathRenderNode* degree, qreal scale) {
  const MathRenderNode* semantic = primarySemanticNode(degree);
  if (!semantic)
    return cssNodeWidth(degree, scale);
  const MathFontConstants& constants =
      OpenTypeMathFont::instance().constants();
  if (semantic->semanticKind == MathSemanticKind::Fraction) {
    const qreal fractionWidth = fractionCssWidth(semantic, scale);
    return 2.0 + std::max<qreal>(0.0, fractionWidth - 2.0) *
                     constants.scriptPercentScaleDown;
  }
  if (semantic->semanticKind == MathSemanticKind::SupSub &&
      semantic->children.size() == 2) {
    const qreal base = cssNodeWidth(semantic->children.front().get(), scale);
    const qreal scripts = cssNodeWidth(semantic->children.back().get(), scale) *
                          constants.scriptPercentScaleDown;
    return snapEighth(base + scripts +
                      constants.spaceAfterScript * mathStyleScale(semantic));
  }
  return cssNodeWidth(degree, scale);
}

qreal radicalDegreeAdvanceWidth(const MathRenderNode* degree, qreal scale) {
  const MathRenderNode* semantic = primarySemanticNode(degree);
  const qreal width = cssNodeWidth(degree, scale);
  if (!semantic) return width;
  const MathFontConstants& constants =
      OpenTypeMathFont::instance().constants();
  if (semantic->semanticKind == MathSemanticKind::Fraction)
    return width * constants.scriptScriptPercentScaleDown;
  if (semantic->semanticKind == MathSemanticKind::SupSub &&
      semantic->children.size() == 2) {
    return std::max<qreal>(
        0.0, radicalDegreePaintWidth(degree, scale) -
                 constants.radicalKernBeforeDegree);
  }
  if (semantic->semanticKind == MathSemanticKind::Radical)
    return std::max<qreal>(
        0.0, width - constants.spaceAfterScript -
                 constants.radicalRuleThickness / 2.0 *
                     mathStyleScale(semantic));
  return width;
}

qreal radicalDegreePaintHeight(const MathRenderNode* degree, qreal scale) {
  qreal height = cssNodeHeight(degree, scale);
  const MathRenderNode* semantic = primarySemanticNode(degree);
  if (semantic && semantic->semanticKind == MathSemanticKind::Fraction) {
    const qreal ruleLeading = OpenTypeMathFont::instance()
                                  .constants()
                                  .fractionRuleThickness *
                              semantic->fractionSizeMultiplier / 4.0;
    height -= ruleLeading;
  } else if (semantic &&
             semantic->semanticKind == MathSemanticKind::Radical) {
    height += OpenTypeMathFont::instance()
                  .constants()
                  .radicalVerticalGap / 2.0 * mathStyleScale(semantic);
  } else if (semantic &&
             semantic->semanticKind == MathSemanticKind::SupSub) {
    const QRectF intrinsicRect(
        0.0, 0.0, radicalDegreePaintWidth(degree, scale), 0.0);
    if (const auto operation = buildScriptOperation(
            semantic, nullptr, intrinsicRect, scale))
      height = operation->container.height() *
               OpenTypeMathFont::instance()
                   .constants()
                   .scriptScriptPercentScaleDown;
  }
  return snapLayoutUnit(height);
}

qreal fractionDelimiterExtent(const MathRenderNode* fraction, qreal minimumExtent) {
  if (!fraction) return {};
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  qreal extent = 0.0;
  for (const QString& delimiter : {fraction->leftDelimiter, fraction->rightDelimiter}) {
    if (delimiter.isEmpty() || delimiter == QLatin1String(".")) continue;
    const auto variant = font.verticalVariant(
        mathDelimiterCharacter(delimiter), minimumExtent);
    if (variant) extent = std::max(extent, variant->extent);
  }
  return extent;
}

qreal radicalCssWidth(const MathRenderNode* radical, qreal scale) {
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const MathFontConstants& constants = font.constants();
  const MathRenderNode* radicalBox = radical->radicalIndex
      ? indexedRadicalBox(radical) : radical;
  const MathRenderNode* degree = radical->radicalIndex
      ? indexedRadicalDegree(radical, radicalBox) : nullptr;
  const auto widthVariant = font.verticalVariant(
      QString(QChar(0x221A)), font.pixelSize() * 1.875);
  const qreal styleScale = mathStyleScale(radical);
  const qreal radicalWidth = widthVariant
      ? (widthVariant->advance + constants.radicalRuleThickness / 2.0) *
            styleScale
      : font.pixelSize() * styleScale;
  const qreal degreeAdvance = degree
      ? std::max<qreal>(0.0, constants.radicalKernBeforeDegree +
                                radicalDegreeAdvanceWidth(degree, scale) +
                                constants.radicalKernAfterDegree)
      : 0.0;
  const qreal degreeWidthContribution = degree
      ? constants.radicalKernBeforeDegree + degreeAdvance
      : 0.0;
  return degreeWidthContribution + radicalWidth +
         cssNodeWidth(radicalBody(radicalBox), scale);
}

const MathRenderNode* indexedRadicalDegree(
    const MathRenderNode* root, const MathRenderNode* radicalBox) {
  if (!root || !radicalBox || root == radicalBox ||
      !containsNode(root, radicalBox))
    return nullptr;
  const MathRenderNode* sibling = nullptr;
  const MathRenderNode* pathChild = nullptr;
  for (const auto& child : root->children) {
    if (!child) continue;
    if (containsNode(child.get(), radicalBox)) {
      pathChild = child.get();
    } else if (!sibling &&
               (primarySemanticNode(child.get()) ||
                singleSymbol(child.get()))) {
      sibling = child.get();
    }
  }
  if (const MathRenderNode* nested =
          indexedRadicalDegree(pathChild, radicalBox))
    return nested;
  return sibling;
}

qreal cssNodeWidth(const MathRenderNode* node, qreal scale) {
  if (node == nullptr) return 0.0;
  if (const MathRenderNode* textRun = wrappedTextModeRun(node)) {
    QString text;
    if (collectTextModeRun(textRun, &text)) {
      if (const auto shaped = shapeTextModeRun(
              text, mathStyleScale(textRun)))
        return shaped->advance;
    }
  }
  if (!node->middleDelimiter.isEmpty()) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const qreal styleScale = mathStyleScale(node);
    const auto glyph = font.glyph(
        mathMiddleDelimiterCharacter(node->middleDelimiter));
    return glyph ? (glyph->advance +
                    2.0 * font.pixelSize() * kMathMlMiddleSpaceEm) *
                       styleScale
                 : node->width * scale;
  }
  if (node->kind == MathRenderKind::Rule || node->text == QLatin1String("nulldelimiter"))
    return 0.0;
  if (node->atomClass == QLatin1String("mspace") && node->text == QLatin1String("glue"))
    return 0.0;
  if (node->operatorKind == MathOperatorKind::Limits) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const bool symbolOperator = hasOperatorKind(node, MathOperatorKind::Symbol);
    const qreal operatorSpacing = symbolOperator
        ? 2.0 * 3.0 / 18.0 * font.pixelSize() : 0.0;
    qreal width = 0.0;
    for (const auto& child : node->children) {
      qreal childWidth = cssNodeWidth(child.get(), scale);
      if (hasOperatorKind(child.get(), MathOperatorKind::Symbol))
        childWidth -= operatorSpacing;
      width = std::max(width, childWidth);
    }
    return width - font.constants().spaceAfterScript / 2.0 +
           operatorSpacing;
  }
  if (node->kind == MathRenderKind::Accent &&
      node->accentKind != MathAccentKind::None && !node->children.empty()) {
    const bool bodyIsLast = node->accentKind == MathAccentKind::Under ||
                            node->accentKind == MathAccentKind::OverBrace;
    const MathRenderNode* body = bodyIsLast ? node->children.back().get()
                                            : node->children.front().get();
    return cssNodeWidth(body, scale);
  }
  if (node->semanticKind == MathSemanticKind::SupSub) {
    if (const MathRenderNode* brace = node->kind == MathRenderKind::VList
            ? firstKind(node, MathRenderKind::Accent) : nullptr;
        brace && (brace->accentKind == MathAccentKind::UnderBrace ||
                  brace->accentKind == MathAccentKind::OverBrace))
      return cssNodeWidth(brace, scale) * mathStyleScale(node);
  }
  if (node->kind == MathRenderKind::LeftRight) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    size_t firstBody = node->leftDelimiter == QLatin1String(".") ? 0 : 1;
    size_t bodyEnd = node->children.size() -
                     (node->rightDelimiter == QLatin1String(".") ? 0 : 1);
    const MathRenderNode* body = firstBody < bodyEnd
        ? node->children[firstBody].get() : nullptr;
    const qreal targetHeight = std::max({
        (node->height + node->depth) * scale,
        cssNodeHeight(body, scale),
        font.pixelSize() * kMathTableRowHeightEm});
    qreal width = arrayDelimiterWidth(node->leftDelimiter, targetHeight) +
                  arrayDelimiterWidth(node->rightDelimiter, targetHeight);
    for (size_t i = firstBody; i < bodyEnd; ++i) {
      const MathRenderNode* bodyChild = node->children[i].get();
      width += middleDelimiterMarker(bodyChild)
          ? leftRightBodyCssWidth(bodyChild, scale)
          : cssNodeWidth(bodyChild, scale);
    }
    return width;
  }
  if (node->semanticKind == MathSemanticKind::Fraction)
    return fractionCssWidth(node, scale);
  if (node->semanticKind == MathSemanticKind::Radical)
    return radicalCssWidth(node, scale);
  if (node->semanticKind == MathSemanticKind::Array)
    return arrayCssWidth(node, scale);
  if (node->semanticKind == MathSemanticKind::SupSub && node->children.size() == 2) {
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    const qreal nodeStyleScale = node->mathStyleSize >= 3
        ? constants.scriptScriptPercentScaleDown
        : node->mathStyleSize >= 2 ? constants.scriptPercentScaleDown : 1.0;
    const qreal base = cssNodeWidth(node->children.front().get(), scale);
    const qreal scripts = cssNodeWidth(node->children.back().get(), scale);
    qreal width = snapEighth(base + scripts + constants.spaceAfterScript * nodeStyleScale);
    return width;
  }
  if (node->kind == MathRenderKind::Symbol) {
    if (const auto glyph = nativeGlyphBox(node)) return glyph->width;
    if (node->fontClass == QLatin1String("main")) {
      if (const auto shaped = OpenTypeMathFont::instance().shapeMathMlText(
              node->text, mathStyleScale(node)))
        return shaped->advance;
    }
    return std::round(node->width * scale);
  }
  if (node->children.empty()) return node->width * scale;
  if (node->kind == MathRenderKind::VList || node->kind == MathRenderKind::Accent) {
    qreal width = 0.0;
    for (const auto& child : node->children)
      width = std::max(width, cssNodeWidth(child.get(), scale));
    return width;
  }
  qreal width = 0.0;
  for (const auto& child : node->children) width += cssNodeWidth(child.get(), scale);
  return width;
}

qreal cssNodeHeight(const MathRenderNode* node, qreal scale) {
  if (node == nullptr) return 0.0;
  if (const MathRenderNode* textRun = wrappedTextModeRun(node)) {
    QString text;
    if (collectTextModeRun(textRun, &text)) {
      if (const auto shaped = shapeTextModeRun(
              text, mathStyleScale(textRun)))
        return shapedTextCssHeight(*shaped);
    }
  }
  if (!node->middleDelimiter.isEmpty()) {
    const QRectF bounds = middleDelimiterLayoutBounds(
        node->middleDelimiter, mathStyleScale(node));
    if (!bounds.isEmpty()) return bounds.height();
  }
  const MathRenderNode* semantic = primarySemanticNode(node);
  if (semantic && semantic != node) {
    qreal height = cssNodeHeight(semantic, scale);
    if (semantic->semanticKind == MathSemanticKind::Fraction)
      height = std::max(height, std::ceil(rootLeftRightFenceExtent(
                                    node, semantic, height)));
    return height;
  }
  if (node->kind == MathRenderKind::Accent &&
      (node->accentKind == MathAccentKind::Under ||
       node->accentKind == MathAccentKind::Over) &&
      !node->accentCharacter.isEmpty() && node->children.size() >= 2) {
    const bool bodyIsLast = node->accentKind == MathAccentKind::Under;
    const MathRenderNode* body = bodyIsLast
        ? node->children.back().get() : node->children.front().get();
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const qreal target = cssNodeWidth(node, scale);
    if (node->accentUsesNaturalWidth) {
      if (const auto variant = font.horizontalVariant(
              node->accentCharacter, 0.0)) {
        return cssNodeHeight(body, scale) + variant->advance +
               font.constants().overbarExtraAscender;
      }
      if (const auto glyph = font.glyph(node->accentCharacter)) {
        return cssNodeHeight(body, scale) +
               std::ceil(glyph->inkBounds.height()) +
               font.constants().overbarExtraAscender;
      }
    }
    const HorizontalAccentSelection selection = selectHorizontalAccent(
        node->accentCharacter, target);
    const qreal arrowHeight = selection.height();
    const MathFontConstants& constants = font.constants();
    return cssNodeHeight(body, scale) + arrowHeight +
           (node->accentKind == MathAccentKind::Under
                ? constants.underbarExtraDescender
                : constants.overbarExtraAscender);
  }
  if (node->semanticKind == MathSemanticKind::SupSub) {
    if (const MathRenderNode* brace = node->kind == MathRenderKind::VList
            ? firstKind(node, MathRenderKind::Accent) : nullptr;
        brace && (brace->accentKind == MathAccentKind::UnderBrace ||
                  brace->accentKind == MathAccentKind::OverBrace))
      return braceAccentCssHeight(node, brace, scale);
    const GlyphInkExtents extents = glyphInkExtents(node, 1.0);
    qreal height = extents.top + extents.bottom;
    if (hasNestedSemantic(node, MathSemanticKind::Fraction) ||
        hasNestedSemantic(node, MathSemanticKind::Radical)) {
      if (const auto operation = buildScriptOperation(
              node, node, QRectF(0.0, 0.0, cssNodeWidth(node, scale), 0.0),
              scale)) {
        qreal top = 0.0;
        qreal bottom = 0.0;
        for (const QRectF component : {operation->base,
                                       operation->superscript,
                                       operation->subscript}) {
          if (component.isEmpty()) continue;
          top = std::min(top, component.top());
          bottom = std::max(bottom, component.bottom());
        }
        height = std::max(height, std::ceil(bottom - top));
      }
    }
    return height;
  }
  if (node->semanticKind == MathSemanticKind::Fraction) {
    const qreal height = fractionMathMlHeight(
        node, OpenTypeMathFont::instance().pixelSize(), scale);
    if (height > 0.0) return height;
    return std::ceil((node->height + node->depth) * scale);
  }
  if (node->semanticKind == MathSemanticKind::Radical) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const MathFontConstants& constants = font.constants();
    const qreal styleScale = mathStyleScale(node);
    const MathRenderNode* body = radicalBody(node->radicalIndex
        ? indexedRadicalBox(node) : node);
    const qreal required = cssNodeHeight(body, scale) -
                           embeddedBraceStyleReduction(body) +
                           (constants.radicalVerticalGap +
                            constants.radicalRuleThickness) * styleScale;
    const auto variant = font.verticalVariant(
        QString(QChar(0x221A)), required / styleScale);
    if (!variant) return std::ceil((node->height + node->depth) * scale);
    return variant->extent * styleScale +
           constants.radicalExtraAscender * styleScale;
  }
  if (node->semanticKind == MathSemanticKind::Array) {
    const qreal height = arrayCssHeight(node, scale);
    if (height > 0.0) return height;
  }
  if (node->kind == MathRenderKind::Symbol) {
    if (const auto glyph = nativeGlyphBox(node)) return glyph->height;
    if (node->fontClass == QLatin1String("main")) {
      if (const auto shaped = OpenTypeMathFont::instance().shapeMathMlText(
              node->text, mathStyleScale(node)))
        return std::ceil(shaped->inkBounds.height());
    }
  }
  if (symbolCount(node) == 1) {
    const MathRenderNode* symbol = singleSymbol(node);
    if (const auto glyph = nativeGlyphBox(symbol)) return glyph->height;
    if (symbol && symbol->text.size() > 1 &&
        symbol->fontClass == QLatin1String("main")) {
      if (const auto shaped = OpenTypeMathFont::instance().shapeMathMlText(
              symbol->text, mathStyleScale(symbol)))
        return std::ceil(shaped->inkBounds.height());
    }
  }
  if ((node->kind == MathRenderKind::Span || node->children.size() == 1) &&
      middleDelimiterMarker(node)) {
    qreal height = 0.0;
    for (const auto& child : node->children)
      height = std::max(height, cssNodeHeight(child.get(), scale));
    return height;
  }
  if (node->semanticKind == MathSemanticKind::None &&
      (node->kind == MathRenderKind::Span ||
       node->kind == MathRenderKind::VList) &&
      containsTextModeRun(node)) {
    qreal height = 0.0;
    for (const auto& child : node->children) {
      if (!child || (symbolCount(child.get()) == 0 &&
                     child->semanticKind == MathSemanticKind::None))
        continue;
      height = std::max(height, cssNodeHeight(child.get(), scale));
    }
    return height;
  }
  return std::ceil((node->height + node->depth) * scale) +
         (hasAtomClass(node, QLatin1StringView("mbin")) ? 1.0 : 0.0);
}

std::optional<qreal> cssNodeOffset(const MathRenderNode* node,
                                   const MathRenderNode* target,
                                   qreal scale) {
  if (!node || !target) return std::nullopt;
  if (node == target) return 0.0;
  qreal sequentialOffset = 0.0;
  const bool sequential = node->kind == MathRenderKind::Span;
  for (const auto& child : node->children) {
    if (!child) continue;
    if (const auto nested = cssNodeOffset(child.get(), target, scale)) {
      const qreal localOffset = sequential
          ? sequentialOffset
          : child->xOffset * scale;
      return localOffset + *nested;
    }
    if (sequential) sequentialOffset += cssNodeWidth(child.get(), scale);
  }
  return std::nullopt;
}

qreal fractionMathMlHeight(const MathRenderNode* fraction, qreal cssFontSize,
                           qreal renderScale) {
  const GlyphInkExtents extents = fractionMathMlExtents(
      fraction, cssFontSize, renderScale);
  return extents.top + extents.bottom;
}

GlyphInkExtents fractionMathMlExtents(const MathRenderNode* fraction,
                                      qreal cssFontSize,
                                      qreal renderScale) {
  const auto metrics = fractionMetrics(fraction, cssFontSize, renderScale);
  return metrics ? metrics->extents : GlyphInkExtents{};
}

std::optional<FractionMetrics> fractionMetrics(const MathRenderNode* fraction,
                                               qreal cssFontSize,
                                               qreal renderScale) {
  if (!fraction) return {};
  const MathRenderNode* stack = directChild(fraction, MathRenderKind::VList);
  if (!stack) return {};
  const MathRenderNode* denominator = nullptr;
  const MathRenderNode* numerator = nullptr;
  for (const auto& child : stack->children) {
    if (!child || child->kind == MathRenderKind::Rule) continue;
    if (!denominator) denominator = child.get();
    numerator = child.get();
  }
  if (!numerator || !denominator || numerator == denominator) return std::nullopt;

  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  const bool displayStyle = fraction->fractionStyleSize == 0;
  const qreal styleScale = fraction->fractionSizeMultiplier;
  GlyphInkExtents numeratorInk = glyphInkExtents(numerator, 1.0);
  GlyphInkExtents denominatorInk = glyphInkExtents(denominator, 1.0);
  // Browser fraction rows allocate the full <mo> line box, not only its ink.
  includeMiddleDelimiterLayoutExtents(numerator, &numeratorInk);
  includeMiddleDelimiterLayoutExtents(denominator, &denominatorInk);
  const auto includeMathMlRowBox = [cssFontSize, renderScale, styleScale, &constants](
                                        const MathRenderNode* child,
                                        GlyphInkExtents* ink,
                                        bool includeOperatorLeading) {
    if (!child || !ink) return;
    if (const MathRenderNode* textRun = wrappedTextModeRun(child)) {
      QString text;
      if (collectTextModeRun(textRun, &text)) {
        if (const auto shaped = shapeTextModeRun(
                text, mathStyleScale(textRun))) {
          const qreal allocation = shapedTextCssHeight(*shaped);
          ink->top += std::max<qreal>(
              0.0, allocation - ink->top - ink->bottom);
        }
      }
      return;
    }
    if (symbolCount(child) <= 1) return;
    if (const MathRenderNode* nested = primarySemanticNode(child);
        nested && nested->semanticKind == MathSemanticKind::Fraction) {
      *ink = fractionMathMlExtents(nested, cssFontSize, renderScale);
      ink->top += std::floor(constants.fractionRuleThickness *
                             nested->fractionSizeMultiplier);
      return;
    }
    if (const MathRenderNode* nested = primarySemanticNode(child);
        nested && nested->semanticKind == MathSemanticKind::Radical) {
      if (hasNestedSemantic(nested, MathSemanticKind::Fraction) ||
          hasNestedSemantic(nested, MathSemanticKind::Radical) ||
          hasNestedSemantic(nested, MathSemanticKind::SupSub)) {
        const qreal radicalScale = mathStyleScale(nested);
        const qreal rowHeight = cssNodeHeight(nested, renderScale) -
            constants.radicalRuleThickness * radicalScale;
        ink->top = std::max<qreal>(0.0, rowHeight - ink->bottom);
      }
      return;
    }
    if (const MathRenderNode* nested = primarySemanticNode(child);
        nested && nested->semanticKind == MathSemanticKind::SupSub) {
      if (const MathRenderNode* brace = firstKind(nested, MathRenderKind::Accent);
          brace && (brace->accentKind == MathAccentKind::UnderBrace ||
                    brace->accentKind == MathAccentKind::OverBrace)) {
        *ink = {braceAccentCssHeight(nested, brace, renderScale), 0.0};
        return;
      }
    }
    if (includeOperatorLeading && hasMathMlOperatorOverflow(child) &&
        !hasMathNumberToken(child))
      ink->top += std::floor(constants.fractionRuleThickness * styleScale);
    if (firstKind(child, MathRenderKind::LeftRight))
      ink->top += std::floor(2.0 * constants.fractionRuleThickness * styleScale);
    if (hasNestedSemantic(child, MathSemanticKind::Fraction) ||
        hasNestedSemantic(child, MathSemanticKind::Radical) ||
        hasNestedSemantic(child, MathSemanticKind::Array)) {
      const qreal rowHeight = cssNodeHeight(child, renderScale);
      ink->top += std::max<qreal>(0.0, rowHeight - ink->top - ink->bottom);
    }
  };
  includeMathMlRowBox(numerator, &numeratorInk, true);
  includeMathMlRowBox(denominator, &denominatorInk, numeratorInk.bottom > 0.0);
  const auto isScriptRow = [](const MathRenderNode* row) {
    const MathRenderNode* semantic = primarySemanticNode(row);
    return semantic && semantic->semanticKind == MathSemanticKind::SupSub;
  };
  const bool hasScriptRow = isScriptRow(numerator) || isScriptRow(denominator);
  qreal topShift = styleScale * (displayStyle
      ? constants.stackTopDisplayStyleShiftUp : constants.stackTopShiftUp);
  qreal bottomShift = styleScale * (displayStyle
      ? constants.stackBottomDisplayStyleShiftDown : constants.stackBottomShiftDown);
  if (!fraction->fractionHasBarLine) {
    const qreal minimumGap = styleScale * (displayStyle
        ? constants.stackDisplayStyleGapMin : constants.stackGapMin);
    const qreal currentGap = topShift + bottomShift - numeratorInk.bottom -
                             denominatorInk.top;
    const qreal adjustment = std::max<qreal>(0.0, minimumGap - currentGap) / 2.0;
    topShift += adjustment;
    bottomShift += adjustment;
    FractionMetrics result;
    result.numerator = numerator;
    result.denominator = denominator;
    result.numeratorInk = numeratorInk;
    result.denominatorInk = denominatorInk;
    result.numeratorShift = topShift;
    result.denominatorShift = bottomShift;
    result.extents = {topShift + numeratorInk.top,
                      bottomShift + denominatorInk.bottom};
    const qreal snappedHeight = std::round(
        (result.extents.top + result.extents.bottom) * 2.0) / 2.0;
    result.extents.bottom += snappedHeight - result.extents.top -
                             result.extents.bottom;
    if (!displayStyle)
      result.extents.top += constants.fractionRuleThickness * styleScale;
    return result;
  }

  topShift = styleScale * (displayStyle
      ? constants.fractionNumeratorDisplayStyleShiftUp
      : constants.fractionNumeratorShiftUp);
  bottomShift = styleScale * (displayStyle
      ? constants.fractionDenominatorDisplayStyleShiftDown
      : constants.fractionDenominatorShiftDown);
  const qreal numeratorGap = styleScale * (displayStyle
      ? constants.fractionNumeratorDisplayStyleGapMin
      : constants.fractionNumeratorGapMin);
  const qreal denominatorGap = styleScale * (displayStyle
      ? constants.fractionDenominatorDisplayStyleGapMin
      : constants.fractionDenominatorGapMin);
  const qreal ruleThickness = fraction->fractionLineThicknessEm >= 0.0
      ? fraction->fractionLineThicknessEm * cssFontSize * styleScale
      : constants.fractionRuleThickness * styleScale;
  const qreal numeratorClearance = topShift - numeratorInk.bottom -
                                   constants.axisHeight * styleScale -
                                   ruleThickness / 2.0;
  topShift += std::max<qreal>(0.0, numeratorGap - numeratorClearance);
  const qreal denominatorClearance = constants.axisHeight * styleScale -
                                     ruleThickness / 2.0 -
                                     denominatorInk.top + bottomShift;
  bottomShift += std::max<qreal>(0.0, denominatorGap - denominatorClearance);
  FractionMetrics result;
  result.numerator = numerator;
  result.denominator = denominator;
  result.numeratorInk = numeratorInk;
  result.denominatorInk = denominatorInk;
  result.numeratorShift = topShift;
  result.denominatorShift = bottomShift;
  result.ruleThickness = ruleThickness;
  result.extents = {
      topShift + numeratorInk.top +
          (hasScriptRow ? constants.fractionRuleThickness * styleScale / 2.0 : 0.0),
      bottomShift + denominatorInk.bottom};
  return result;
}

qreal rootLeftRightFenceExtent(const MathRenderNode* node,
                               const MathRenderNode* rootSemantic,
                               qreal minimumExtent) {
  if (!node) return 0.0;
  if (node->kind == MathRenderKind::LeftRight &&
      primarySemanticNode(node) == rootSemantic) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    qreal extent = 0.0;
    for (const QString& delimiter : {node->leftDelimiter, node->rightDelimiter}) {
      if (delimiter.isEmpty() || delimiter == QLatin1String(".")) continue;
      const QString character = mathDelimiterCharacter(delimiter);
      if (!mathMlOperatorIsStretchy(character)) {
        if (const auto base = font.glyph(character))
          extent = std::max(extent, base->inkBounds.height());
        continue;
      }
      const MathRenderNode* fenceSemantic = primarySemanticNode(node);
      const bool middleFraction = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::Fraction &&
          middleDelimiterMarker(fenceSemantic);
      if (middleFraction) {
        extent = std::max(
            extent,
            std::floor(minimumExtent) + kChromiumMathAxisOffsetPx);
        continue;
      }
      const bool middleRadical = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::Radical &&
          middleDelimiterMarker(fenceSemantic);
      if (middleRadical) {
        extent = std::max(
            extent,
            std::ceil(minimumExtent) +
                2.0 * std::ceil(font.constants().fractionRuleThickness));
        continue;
      }
      const bool middleScript = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::SupSub &&
          middleDelimiterMarker(fenceSemantic);
      if (middleScript) {
        extent = std::max(
            extent,
            std::ceil(minimumExtent) +
                4.0 * std::ceil(font.constants().fractionRuleThickness) +
                std::ceil(font.constants().fractionRuleThickness / 2.0));
        continue;
      }
      const bool middleArray = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::Array &&
          middleDelimiterMarker(fenceSemantic);
      if (middleArray) {
        extent = std::max(
            extent,
            std::ceil(minimumExtent) +
                std::ceil(font.constants().fractionRuleThickness));
        continue;
      }
      const bool textStyleFraction = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::Fraction &&
          fenceSemantic->fractionStyleSize > 0;
      if (textStyleFraction) {
        if (const auto variant = font.verticalVariant(
                character, minimumExtent, true))
          extent = std::max(extent, variant->extent);
        continue;
      }
      const bool tallFence =
          minimumExtent > font.pixelSize() * kMathTableRowHeightEm;
      const qreal fenceLeading = 2.0 * std::ceil(
          font.constants().fractionRuleThickness);
      const auto largestFixed = font.verticalVariant(
          character, std::numeric_limits<qreal>::max());
      const bool requiresAssembly = largestFixed &&
          minimumExtent > largestFixed->extent;
      const bool fractionFence = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::Fraction;
      const bool textArrayFence = fenceSemantic &&
          fenceSemantic->semanticKind == MathSemanticKind::Array &&
          containsTextModeRun(fenceSemantic);
      const bool curvedFence = character == QLatin1String("(") ||
                               character == QLatin1String(")");
      const bool ceilingTerminal =
          character == QString(QChar(0x2308)) ||
          character == QString(QChar(0x2309));
      const qreal targetExtent = ceilingTerminal
          ? minimumExtent
          : minimumExtent + (requiresAssembly
                ? fractionFence
                    ? 2.0 * font.constants().axisHeight +
                          font.constants().fractionRuleThickness / 2.0
                    : textArrayFence
                        ? curvedFence
                            ? font.constants().fractionRuleThickness
                            : font.constants().fractionRuleThickness / 2.0
                    : font.constants().fractionRuleThickness / 2.0
                : tallFence ? fenceLeading : 0.0);
      const auto variant = font.verticalVariant(character, targetExtent, true);
      if (variant) {
        const bool assembled = largestFixed &&
            variant->extent > largestFixed->extent + 0.001;
        qreal visualExtent = variant->extent;
        if (tallFence && !assembled)
          visualExtent = std::round(visualExtent) + fenceLeading;
        extent = std::max(extent, visualExtent);
      }
    }
    return extent;
  }
  qreal extent = 0.0;
  for (const auto& child : node->children)
    extent = std::max(extent, rootLeftRightFenceExtent(
                                  child.get(), rootSemantic, minimumExtent));
  return extent;
}

qreal siblingSemanticHeight(const MathRenderNode* node,
                            const MathRenderNode* primary,
                            qreal scale, qreal cssFontSize) {
  if (!node || node == primary) return 0.0;
  if (node->semanticKind == MathSemanticKind::Fraction)
    return fractionMathMlHeight(node, cssFontSize, scale);
  if (node->semanticKind != MathSemanticKind::None)
    return cssNodeHeight(node, scale);
  qreal height = 0.0;
  for (const auto& child : node->children)
    height = std::max(
        height, siblingSemanticHeight(child.get(), primary, scale,
                                      cssFontSize));
  return height;
}

bool hasSiblingAtomClass(const MathRenderNode* node,
                         const MathRenderNode* primary,
                         QLatin1StringView atomClass) {
  if (!node || node == primary) return false;
  if (node->atomClass == atomClass) return true;
  for (const auto& child : node->children)
    if (hasSiblingAtomClass(child.get(), primary, atomClass)) return true;
  return false;
}

qreal heightOutsideAccent(const MathRenderNode* node,
                          const MathRenderNode* accent,
                          qreal scale) {
  if (!node || node == accent) return 0.0;
  if (node->kind == MathRenderKind::Symbol) {
    const GlyphInkExtents ink = glyphInkExtents(node, 1.0);
    return ink.top + ink.bottom;
  }
  qreal height = 0.0;
  for (const auto& child : node->children)
    height = std::max(height, heightOutsideAccent(child.get(), accent, scale));
  return height;
}

qreal accentBodyCssHeight(const MathRenderNode* body, qreal scale) {
  if (!body) return 0.0;
  if (body->kind == MathRenderKind::Accent)
    return cssNodeHeight(body, scale);
  if (const MathRenderNode* nested = firstKind(
          body, MathRenderKind::Accent);
      nested && (nested->accentKind == MathAccentKind::Under ||
                 nested->accentKind == MathAccentKind::Over) &&
      !nested->accentCharacter.isEmpty())
    return cssNodeHeight(nested, scale);
  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  const MathRenderNode* semantic = primarySemanticNode(body);
  if (!semantic) {
    if (const auto glyph = nativeGlyphBox(singleSymbol(body)))
      return glyph->height;
    const GlyphInkExtents ink = glyphInkExtents(body, 1.0);
    qreal height = ink.top + ink.bottom;
    if (hasMathMlOperatorOverflow(body))
      height += std::floor(constants.fractionRuleThickness);
    return std::ceil(height);
  }

  qreal height = cssNodeHeight(body, scale);
  const bool directSemanticBody =
      symbolCount(body) == symbolCount(semantic);
  if (directSemanticBody &&
      semantic->semanticKind == MathSemanticKind::Fraction &&
      semantic->fractionHasBarLine) {
    height -= constants.fractionRuleThickness *
              semantic->fractionSizeMultiplier;
  } else if (directSemanticBody &&
             semantic->semanticKind == MathSemanticKind::Radical) {
    // Embedded MathML blocks contribute their ink extent to mover/munder;
    // Chromium excludes the root inline-block extra ascender.
    height -= constants.radicalExtraAscender -
              constants.radicalRuleThickness / 2.0;
  }
  return height;
}

qreal braceAccentCssHeight(const MathRenderNode* container,
                           const MathRenderNode* accent,
                           qreal scale) {
  if (!container || !accent || accent->children.size() < 2) return 0.0;
  const bool over = accent->accentKind == MathAccentKind::OverBrace;
  const MathRenderNode* body = over ? accent->children.back().get()
                                    : accent->children.front().get();
  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  const qreal bodyHeight = accentBodyCssHeight(body, scale);
  const qreal gap = over ? constants.overbarVerticalGap
                         : constants.underbarVerticalGap;
  const qreal rule = over ? constants.overbarRuleThickness
                          : constants.underbarRuleThickness;
  qreal braceHeight = !over && containsTextModeRun(body)
      ? std::ceil(2.0 * rule)
      : std::round(gap + 2.0 * rule);
  braceHeight -= embeddedBraceStyleReduction(body);
  const qreal extra = over ? constants.overbarExtraAscender
                           : constants.underbarExtraDescender;
  qreal height = bodyHeight + gap + braceHeight + extra;
  const MathRenderNode* annotation = nullptr;
  for (const auto& child : container->children) {
    if (child && !containsNode(child.get(), accent)) {
      annotation = child.get();
      break;
    }
  }
  const MathRenderNode* annotationSemantic =
      primarySemanticNode(annotation);
  const bool textModeAnnotation = containsTextModeRun(annotation);
  qreal annotationHeight = textModeAnnotation
      ? cssNodeHeight(annotation, scale)
      : annotationSemantic
      ? cssNodeHeight(annotation, scale)
      : heightOutsideAccent(annotation, accent, scale);
  if (annotationSemantic &&
      annotationSemantic->semanticKind == MathSemanticKind::Fraction &&
      annotationSemantic->fractionHasBarLine)
    annotationHeight -= constants.fractionRuleThickness;
  if (!annotationSemantic && !textModeAnnotation) {
    annotationHeight = std::min(
        annotationHeight, std::ceil(constants.axisHeight));
    if (annotationHeight > 0.0)
      annotationHeight += std::floor(constants.fractionRuleThickness);
  }
  if (annotationHeight > 0.0)
    height += gap + annotationHeight + extra;
  const qreal styleScale = mathStyleScale(container);
  if (styleScale < 1.0) {
    const auto roundedComponent = [styleScale](qreal component) {
      const qreal scaled = component * styleScale;
      return std::ceil(scaled) - scaled;
    };
    height = height * styleScale + roundedComponent(bodyHeight) +
             roundedComponent(braceHeight) +
             roundedComponent(annotationHeight);
    height = snapLayoutUnit(height);
  }
  return height;
}

const MathRenderNode* radicalBody(const MathRenderNode* radical) {
  if (radical == nullptr) return nullptr;
  for (const auto& child : radical->children)
    if (child && child->kind != MathRenderKind::Stretchy) return child.get();
  return nullptr;
}

}  // namespace muffin::math::mathml
