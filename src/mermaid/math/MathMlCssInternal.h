#pragma once

// Internal contract between MathML measurement and paint construction. These
// helpers operate on immutable render trees; public consumers use MathMlCssLayout.h.
#include "mermaid/math/MathMlCssLayout.h"
#include "math/OpenTypeMathFont.h"

#include <algorithm>
#include <cmath>

namespace muffin::math::mathml {

inline constexpr qreal kChromiumMathAxisOffsetPx = 6.0;
inline constexpr qreal kMathTableRowHeightEm = 1.22265625;
inline constexpr qreal kMathTableDescenderExpansionEm = 0.25;
inline constexpr qreal kMathTableTextDescenderExpansionEm = 3.0 / 16.0;
inline constexpr qreal kMathTableExtraColumnCorrectionPx = 1.8;
inline constexpr qreal kMathTableCellVerticalPaddingEm = 0.47265625;
inline constexpr qreal kMathTableCellHorizontalPaddingEm = 0.8;
inline constexpr qreal kMathMlThickSpaceEm = 5.0 / 18.0;
inline constexpr qreal kMathMlMiddleSpaceEm = 0.05;

struct HorizontalAccentSelection {
  std::optional<MathGlyphVariant> fixed;
  std::optional<MathGlyphAssembly> assembly;
  std::optional<MathShapedTextRun> text;

  qreal height() const {
    if (text) return std::ceil(text->inkBounds.height());
    if (fixed) return std::ceil(fixed->advance);
    return assembly ? std::ceil(assembly->inkBounds.height()) : 0.0;
  }

};

struct NativeGlyphBox {
  qreal width = 0.0;
  qreal height = 0.0;
};

struct GlyphInkExtents {
  qreal top = 0.0;
  qreal bottom = 0.0;
};

struct FractionMetrics {
  const MathRenderNode* numerator = nullptr;
  const MathRenderNode* denominator = nullptr;
  GlyphInkExtents numeratorInk;
  GlyphInkExtents denominatorInk;
  qreal numeratorShift = 0.0;
  qreal denominatorShift = 0.0;
  qreal ruleThickness = 0.0;
  GlyphInkExtents extents;
};

struct MathMlStyleContext {
  int scriptLevel = 0;

  qreal fontScale() const {
    const MathFontConstants& constants =
        OpenTypeMathFont::instance().constants();
    if (scriptLevel >= 2) return constants.scriptScriptPercentScaleDown;
    if (scriptLevel == 1) return constants.scriptPercentScaleDown;
    return 1.0;
  }

  MathMlStyleContext scriptStyle() const {
    return {std::min(scriptLevel + 1, 2)};
  }
};

struct CssTokenLineMetrics {
  GlyphInkExtents ink;
  qreal ascent = 0.0;
  qreal descent = 0.0;

  qreal height() const { return ascent + descent; }
};

qreal snapEighth(qreal value);

qreal snapLayoutUnit(qreal value);

qreal mathMlInlineAscent(qreal fontScale = 1.0);

qreal mathStyleScale(const MathRenderNode* node);

QRectF snapVerticalLayoutRect(QRectF rect);

const MathRenderNode* primarySemanticNode(const MathRenderNode* node);

qreal widestDescendant(const MathRenderNode* node, MathRenderKind kind);

const MathRenderNode* directChild(const MathRenderNode* node, MathRenderKind kind);

bool hasNestedSemantic(const MathRenderNode* node, MathSemanticKind kind);

const MathRenderNode* nestedSemanticNode(const MathRenderNode* node,
                                         MathSemanticKind kind);

HorizontalAccentSelection selectHorizontalAccent(
    const QString& character, qreal target, bool allowAssembly = false);

QString mathDelimiterCharacter(const QString& delimiter);

QString mathMiddleDelimiterCharacter(const QString& delimiter);

QRectF middleDelimiterLayoutBounds(const QString& delimiter,
                                   qreal fontScale);

qreal arrayDelimiterWidth(const QString& delimiter, qreal targetHeight);

const MathRenderNode* middleDelimiterMarker(const MathRenderNode* node,
                                            bool root = true);

const MathRenderNode* indexedRadicalBox(const MathRenderNode* node);

bool findNodePath(const MathRenderNode* current,
                  const MathRenderNode* target,
                  const QString& currentPath,
                  QString* result);

QString nodePath(const MathRenderNode* root, const MathRenderNode* target);

QString expectedMathMlTag(const MathRenderNode* node);

qreal leftRightBodyCssWidth(const MathRenderNode* node, qreal scale,
                            bool root = true);

const MathRenderNode* arrayTableBody(const MathRenderNode* node, int columns);

qreal arrayCssWidth(const MathRenderNode* array, qreal scale);

const MathRenderNode* columnRows(const MathRenderNode* node, int rows);

const MathRenderNode* arrayCellNode(const MathRenderNode* array,
                                    const MathRenderNode* table,
                                    int row, int column);

bool hasPlainLatinDescender(const MathRenderNode* node);

bool hasMathMlOperatorOverflow(const MathRenderNode* node);

bool hasMathNumberToken(const MathRenderNode* node);

bool hasOperatorKind(const MathRenderNode* node, MathOperatorKind kind);

qreal embeddedBraceStyleReduction(const MathRenderNode* node);

bool hasUnderBrace(const MathRenderNode* node);

QVector<qreal> arrayCssRowHeights(const MathRenderNode* array,
                                  const MathRenderNode* table,
                                  qreal scale);

qreal arrayCssHeight(const MathRenderNode* array, qreal scale);

qreal fractionCssWidth(const MathRenderNode* fraction, qreal scale);

qreal radicalDegreePaintWidth(const MathRenderNode* degree, qreal scale);

qreal radicalDegreeAdvanceWidth(const MathRenderNode* degree, qreal scale);

qreal radicalDegreePaintHeight(const MathRenderNode* degree, qreal scale);

qreal fractionDelimiterExtent(const MathRenderNode* fraction, qreal minimumExtent);

qreal radicalCssWidth(const MathRenderNode* radical, qreal scale);

int symbolCount(const MathRenderNode* node);

bool hasAtomClass(const MathRenderNode* node, QLatin1StringView atomClass);

bool isIntegralOperator(const QString& character);

qreal largeOperatorAllocationHeight(const QString& character,
                                    const MathGlyphVariant& variant,
                                    qreal fontScale = 1.0);

qreal maxLargeOperatorExtent(const MathRenderNode* node);

const MathRenderNode* firstKind(const MathRenderNode* node, MathRenderKind kind);

bool containsNode(const MathRenderNode* root, const MathRenderNode* target);

const MathRenderNode* indexedRadicalDegree(
    const MathRenderNode* root, const MathRenderNode* radicalBox);

const MathRenderNode* enclosingKind(const MathRenderNode* node,
                                    const MathRenderNode* target,
                                    MathRenderKind kind);

const MathRenderNode* singleSymbol(const MathRenderNode* node);

const MathRenderNode* wrappedTextModeRun(const MathRenderNode* node);

bool containsTextModeRun(const MathRenderNode* node);

bool collectTextModeRun(const MathRenderNode* node, QString* text,
                        const MathRenderNode** firstSymbol = nullptr);

std::optional<MathShapedText> shapeTextModeRun(const QString& text,
                                               qreal fontScale);

qreal shapedTextCssHeight(const MathShapedText& shaped);

void includeMiddleDelimiterLayoutExtents(const MathRenderNode* node,
                                         GlyphInkExtents* extents,
                                         bool root = true);

GlyphInkExtents shapedTextCssExtents(const MathShapedText& shaped);

bool hasTextModeDescender(const MathRenderNode* node);

GlyphInkExtents glyphInkExtents(const MathRenderNode* node, qreal fontScale);

qreal nativeOperatorPadding(const MathRenderNode* symbol);

std::optional<NativeGlyphBox> nativeGlyphBox(const MathRenderNode* symbol);

qreal cssNodeWidth(const MathRenderNode* node, qreal scale);

qreal cssNodeHeight(const MathRenderNode* node, qreal scale);

std::optional<qreal> cssNodeOffset(const MathRenderNode* node,
                                   const MathRenderNode* target,
                                   qreal scale);

qreal fractionMathMlHeight(const MathRenderNode* fraction, qreal cssFontSize,
                           qreal renderScale);

GlyphInkExtents fractionMathMlExtents(const MathRenderNode* fraction,
                                      qreal cssFontSize,
                                      qreal renderScale);

std::optional<FractionMetrics> fractionMetrics(const MathRenderNode* fraction,
                                               qreal cssFontSize,
                                               qreal renderScale);

bool mathMlOperatorIsStretchy(const QString& character);

qreal rootLeftRightFenceExtent(const MathRenderNode* node,
                               const MathRenderNode* rootSemantic,
                               qreal minimumExtent);

qreal siblingSemanticHeight(const MathRenderNode* node,
                            const MathRenderNode* primary,
                            qreal scale, qreal cssFontSize);

bool hasSiblingAtomClass(const MathRenderNode* node,
                         const MathRenderNode* primary,
                         QLatin1StringView atomClass);

qreal heightOutsideAccent(const MathRenderNode* node,
                          const MathRenderNode* accent,
                          qreal scale);

qreal accentBodyCssHeight(const MathRenderNode* body, qreal scale);

qreal braceAccentCssHeight(const MathRenderNode* container,
                           const MathRenderNode* accent,
                           qreal scale);

const MathRenderNode* radicalBody(const MathRenderNode* radical);

MathCssBoxRole roleFor(const MathRenderNode& node);

MathCssBox convertTree(const MathRenderNode& node, qreal scale);

QString basicAccentCharacter(const MathRenderNode* accent);

bool hasPaintOperation(MathSemanticKind kind);

bool isSupportedHorizontalAccent(const MathRenderNode* node);

const MathRenderNode* ownedHorizontalAccent(const MathRenderNode* node);

bool ownsAccentPaintOperation(const MathRenderNode* node);

bool ownsGenericPaintOperation(const MathRenderNode* node);

bool ownsPaintOperation(const MathRenderNode* node);

bool findPaintOrigin(const MathRenderNode* node,
                     const MathRenderNode* target, QPointF origin,
                     QPointF* result);

bool findCssPaintX(const MathRenderNode* node,
                   const MathRenderNode* target, qreal origin,
                   qreal* result);

bool collectGlyphRunSymbols(const MathRenderNode* node,
                            QVector<const MathRenderNode*>* symbols,
                            bool skipOwnedOperations = false);

std::optional<GlyphInkExtents> paintOutlineInkExtents(
    const MathRenderNode* node, qreal fontScale,
    bool allowPartialOwnership = false);

std::optional<QVector<MathCssGlyphRunOperation>> buildGlyphRunOperations(
    const MathRenderNode* node, QRectF target, qreal fontScale,
    bool allowPartialOwnership = false,
    std::optional<qreal> cssPositionScale = std::nullopt);

MathMlStyleContext fractionRowStyle(
    const MathRenderNode* fraction, MathMlStyleContext parentStyle);

void collectImmediatePaintNodes(const MathRenderNode* node,
                                QVector<const MathRenderNode*>* operations);

GlyphInkExtents preciseTokenInkExtents(const MathRenderNode* node,
                                       qreal fontScale);

CssTokenLineMetrics cssTokenLineMetrics(const MathRenderNode* node,
                                        qreal renderScale,
                                        qreal fontScale);

std::optional<MathCssScriptOperation> buildScriptOperation(
    const MathRenderNode* script, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale);

std::optional<MathCssVerticalGlyphOperation> buildVerticalGlyphOperation(
    const QString& delimiter, QRectF target,
    bool normalizeDelimiter = true);

std::optional<MathCssVerticalGlyphOperation> buildInlineFenceGlyphOperation(
    const QString& delimiter, QRectF target);

std::optional<MathCssRadicalOperation> buildRadicalOperation(
    const MathRenderNode* radical, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale, bool topLevel = false);

qreal nestedFractionCssHeight(const MathRenderNode* fraction,
                              qreal cssRootFontPixelSize,
                              qreal renderScale);

std::optional<MathCssPaintOperation> buildMiddlePaintOperation(
    const MathRenderNode* marker, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale);

const MathRenderNode* leftRightBody(const MathRenderNode* leftRight);

std::optional<MathCssPaintOperation> buildLeftRightOperation(
    const MathRenderNode* leftRight, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale, qreal cssRootFontPixelSize,
    MathMlStyleContext style);

std::optional<MathCssPaintOperation> buildAccentOperation(
    const MathRenderNode* owner, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale, qreal cssRootFontPixelSize,
    MathMlStyleContext style, bool topLevel = false,
    bool rootRowChild = false);

std::optional<MathCssPaintOperation> buildArrayOperation(
    const MathRenderNode* array, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale, qreal cssRootFontPixelSize,
    MathMlStyleContext style);

std::optional<MathCssPaintOperation> buildFractionOperation(
    const MathRenderNode* fraction, const MathRenderNode* containingNode,
    qreal fractionOffset, qreal semanticTop, qreal semanticHeight,
    qreal renderScale, qreal cssRootFontPixelSize, bool nested,
    MathMlStyleContext style);

std::optional<MathCssPaintOperation> buildPaintOperation(
    const MathRenderNode* operationNode, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale, qreal cssRootFontPixelSize,
    MathMlStyleContext style = {}, bool topLevel = false, bool rootRowChild = false);

}  // namespace muffin::math::mathml
