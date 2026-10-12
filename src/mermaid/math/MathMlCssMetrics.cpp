#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math::mathml {

qreal snapEighth(qreal value) {
  return std::round(value * 8.0) / 8.0;
}

qreal snapLayoutUnit(qreal value) {
  return std::round(value * 64.0) / 64.0;
}

qreal mathMlInlineAscent(qreal fontScale) {
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const qreal lineHeight = font.pixelSize() * kMathTableRowHeightEm *
                           fontScale;
  const qreal ascent = std::round(font.typographicAscent() * fontScale);
  const qreal descent = std::round(font.typographicDescent() * fontScale);
  const qreal leading = std::max<qreal>(
      0.0, lineHeight - ascent - descent);
  return ascent + leading / 2.0;
}

qreal mathStyleScale(const MathRenderNode* node) {
  if (!node) return 1.0;
  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  return node->mathStyleSize >= 3
      ? constants.scriptScriptPercentScaleDown
      : node->mathStyleSize >= 2 ? constants.scriptPercentScaleDown : 1.0;
}

QRectF snapVerticalLayoutRect(QRectF rect) {
  const qreal top = snapLayoutUnit(rect.top());
  const qreal height = snapLayoutUnit(rect.height());
  rect.setY(top);
  rect.setHeight(height);
  return rect;
}

const MathRenderNode* primarySemanticNode(const MathRenderNode* node) {
  if (node == nullptr) return nullptr;
  if (node->semanticKind != MathSemanticKind::None) return node;
  for (const auto& child : node->children)
    if (const auto* result = primarySemanticNode(child.get())) return result;
  return nullptr;
}

qreal widestDescendant(const MathRenderNode* node, MathRenderKind kind) {
  if (node == nullptr) return 0.0;
  qreal width = node->kind == kind ? node->width : 0.0;
  for (const auto& child : node->children)
    width = std::max(width, widestDescendant(child.get(), kind));
  return width;
}

const MathRenderNode* directChild(const MathRenderNode* node, MathRenderKind kind) {
  if (node == nullptr) return nullptr;
  for (const auto& child : node->children)
    if (child && child->kind == kind) return child.get();
  return nullptr;
}

bool hasNestedSemantic(const MathRenderNode* node, MathSemanticKind kind) {
  if (node == nullptr) return false;
  for (const auto& child : node->children) {
    if (!child) continue;
    if (child->semanticKind == kind || hasNestedSemantic(child.get(), kind)) return true;
  }
  return false;
}

const MathRenderNode* nestedSemanticNode(const MathRenderNode* node,
                                         MathSemanticKind kind) {
  if (node == nullptr) return nullptr;
  for (const auto& child : node->children) {
    if (!child) continue;
    if (child->semanticKind == kind) return child.get();
    if (const auto* result = nestedSemanticNode(child.get(), kind)) return result;
  }
  return nullptr;
}

HorizontalAccentSelection selectHorizontalAccent(
    const QString& character, qreal target, bool allowAssembly) {
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  HorizontalAccentSelection result;
  if (character.size() != 1) {
    result.text = font.shapeMathMlText(character);
    return result;
  }
  const auto fixed = font.horizontalVariant(
      character, allowAssembly ? target : 0.0);
  if (!allowAssembly) {
    result.fixed = fixed;
    return result;
  }
  const bool fixedReachesTarget = fixed && fixed->extent >= target;
  const auto assembly = fixedReachesTarget
      ? std::optional<MathGlyphAssembly>{}
      : font.horizontalAssemblyParts(character, target);
  if (fixed && (fixedReachesTarget || !assembly)) result.fixed = fixed;
  else result.assembly = assembly;
  return result;
}

QString mathDelimiterCharacter(const QString& delimiter) {
  if (delimiter == QLatin1String("\\lbrace")) return QStringLiteral("{");
  if (delimiter == QLatin1String("\\rbrace")) return QStringLiteral("}");
  if (delimiter == QLatin1String("|") ||
      delimiter == QLatin1String("\\vert") ||
      delimiter == QLatin1String("\\lvert") ||
      delimiter == QLatin1String("\\rvert"))
    return QString(QChar(0x2223));
  if (delimiter == QString(QChar(0x2016)) ||
      delimiter == QLatin1String("\\|") ||
      delimiter == QLatin1String("\\Vert") ||
      delimiter == QLatin1String("\\lVert") ||
      delimiter == QLatin1String("\\rVert"))
    return QString(QChar(0x2225));
  return delimiter;
}

QString mathMiddleDelimiterCharacter(const QString& delimiter) {
  if (delimiter == QLatin1String("|")) return delimiter;
  return mathDelimiterCharacter(delimiter);
}

QRectF middleDelimiterLayoutBounds(const QString& delimiter,
                                   qreal fontScale) {
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const auto glyph = font.glyph(mathMiddleDelimiterCharacter(delimiter));
  if (!glyph || fontScale <= 0.0) return {};
  const QRectF& ink = glyph->inkBounds;
  // Match the baseline-relative pixel rows used by the browser's <mo> box
  // without feeding backend-specific hinted QRawFont bounds into layout.
  const qreal top = std::floor(ink.top() * fontScale);
  const qreal bottom = std::round(ink.bottom() * fontScale);
  return QRectF(ink.x() * fontScale, top, ink.width() * fontScale,
                std::max<qreal>(0.0, bottom - top));
}

const MathRenderNode* middleDelimiterMarker(const MathRenderNode* node,
                                            bool root) {
  if (!node) return nullptr;
  if (!node->middleDelimiter.isEmpty()) return node;
  if (!root && node->kind == MathRenderKind::LeftRight) return nullptr;
  for (const auto& child : node->children)
    if (const MathRenderNode* marker =
            middleDelimiterMarker(child.get(), false))
      return marker;
  return nullptr;
}

bool findNodePath(const MathRenderNode* current,
                  const MathRenderNode* target,
                  const QString& currentPath,
                  QString* result) {
  if (!current || !target) return false;
  if (current == target) {
    *result = currentPath;
    return true;
  }
  for (size_t index = 0; index < current->children.size(); ++index) {
    if (findNodePath(current->children[index].get(), target,
                     currentPath + QLatin1Char('/') +
                         QString::number(index),
                     result))
      return true;
  }
  return false;
}

QString nodePath(const MathRenderNode* root, const MathRenderNode* target) {
  QString result;
  return findNodePath(root, target, QStringLiteral("$"), &result)
      ? result : QStringLiteral("$");
}

QString expectedMathMlTag(const MathRenderNode* node) {
  if (!node) return {};
  switch (node->semanticKind) {
    case MathSemanticKind::Fraction: return QStringLiteral("mfrac");
    case MathSemanticKind::Radical:
      return node->radicalIndex ? QStringLiteral("mroot")
                                : QStringLiteral("msqrt");
    case MathSemanticKind::SupSub:
      switch (node->scriptKind) {
        case MathScriptKind::Superscript: return QStringLiteral("msup");
        case MathScriptKind::Subscript: return QStringLiteral("msub");
        case MathScriptKind::SubSup: return QStringLiteral("msubsup");
        case MathScriptKind::None: break;
      }
      return QStringLiteral("mrow");
    case MathSemanticKind::Array: return QStringLiteral("mtable");
    case MathSemanticKind::None: break;
  }
  if (node->kind == MathRenderKind::Accent)
    return node->accentKind == MathAccentKind::Under ||
                   node->accentKind == MathAccentKind::Underline ||
                   node->accentKind == MathAccentKind::UnderBrace
        ? QStringLiteral("munder") : QStringLiteral("mover");
  if (node->kind == MathRenderKind::LeftRight ||
      node->kind == MathRenderKind::Span)
    return QStringLiteral("mrow");
  return {};
}

bool hasPlainLatinDescender(const MathRenderNode* node) {
  if (node == nullptr || primarySemanticNode(node)) return false;
  if (node->kind == MathRenderKind::Symbol) {
    for (QChar character : node->text)
      if (QStringLiteral("gjpqy").contains(character, Qt::CaseInsensitive)) return true;
  }
  for (const auto& child : node->children)
    if (hasPlainLatinDescender(child.get())) return true;
  return false;
}

bool hasMathMlOperatorOverflow(const MathRenderNode* node) {
  if (!node) return false;
  if (node->kind == MathRenderKind::Symbol) {
    if (node->text == QLatin1String("+")) return true;
    if (node->atomClass == QLatin1String("mrel") && node->text.size() == 1 &&
        node->text.front().unicode() > 0x7f)
      return true;
  }
  for (const auto& child : node->children)
    if (hasMathMlOperatorOverflow(child.get())) return true;
  return false;
}

bool hasMathNumberToken(const MathRenderNode* node) {
  if (!node) return false;
  if (node->kind == MathRenderKind::Symbol && !node->text.isEmpty() &&
      std::all_of(node->text.cbegin(), node->text.cend(),
                  [](QChar character) { return character.isNumber(); }))
    return true;
  for (const auto& child : node->children)
    if (hasMathNumberToken(child.get())) return true;
  return false;
}

bool hasOperatorKind(const MathRenderNode* node, MathOperatorKind kind) {
  if (!node) return false;
  if (node->operatorKind == kind) return true;
  for (const auto& child : node->children)
    if (hasOperatorKind(child.get(), kind)) return true;
  return false;
}

qreal embeddedBraceStyleReduction(const MathRenderNode* node) {
  const MathRenderNode* accent = firstKind(node, MathRenderKind::Accent);
  if (!accent || (accent->accentKind != MathAccentKind::UnderBrace &&
                  accent->accentKind != MathAccentKind::OverBrace))
    return 0.0;
  const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
  const qreal gap = accent->accentKind == MathAccentKind::UnderBrace
      ? constants.underbarVerticalGap : constants.overbarVerticalGap;
  const qreal rule = accent->accentKind == MathAccentKind::UnderBrace
      ? constants.underbarRuleThickness : constants.overbarRuleThickness;
  const qreal fullHeight = std::round(gap + 2.0 * rule);
  return fullHeight -
         std::floor(fullHeight * constants.scriptPercentScaleDown);
}

bool hasUnderBrace(const MathRenderNode* node) {
  const MathRenderNode* accent = firstKind(node, MathRenderKind::Accent);
  return accent && accent->accentKind == MathAccentKind::UnderBrace;
}

int symbolCount(const MathRenderNode* node) {
  if (node == nullptr) return 0;
  int count = node->kind == MathRenderKind::Symbol ? 1 : 0;
  for (const auto& child : node->children) count += symbolCount(child.get());
  return count;
}

bool hasAtomClass(const MathRenderNode* node, QLatin1StringView atomClass) {
  if (node == nullptr) return false;
  if (node->atomClass == atomClass) return true;
  for (const auto& child : node->children)
    if (hasAtomClass(child.get(), atomClass)) return true;
  return false;
}

bool isIntegralOperator(const QString& character) {
  if (character.size() != 1) return false;
  const ushort codepoint = character.front().unicode();
  return (codepoint >= 0x222B && codepoint <= 0x2233) ||
         (codepoint >= 0x2A0B && codepoint <= 0x2A1C);
}

qreal largeOperatorAllocationHeight(const QString& character,
                                    const MathGlyphVariant& variant,
                                    qreal fontScale) {
  const qreal mathExtent = std::ceil(variant.extent * fontScale);
  if (isIntegralOperator(character)) return mathExtent;
  const QRectF rasterInk = OpenTypeMathFont::instance().rasterGlyphBounds(
      variant.glyphIndex, fontScale);
  const qreal rasterLineHeight =
      std::ceil(std::max<qreal>(0.0, -rasterInk.top())) +
      std::ceil(std::max<qreal>(0.0, rasterInk.bottom()));
  return std::max(mathExtent, rasterLineHeight);
}

qreal maxLargeOperatorExtent(const MathRenderNode* node) {
  if (node == nullptr) return 0.0;
  qreal extent = 0.0;
  if (node->kind == MathRenderKind::Symbol && node->atomClass == QLatin1String("mop")) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const auto variant = font.verticalVariant(node->text,
                                              font.constants().displayOperatorMinHeight);
    if (variant)
      extent = isIntegralOperator(node->text)
          ? variant->extent
          : largeOperatorAllocationHeight(node->text, *variant);
  }
  for (const auto& child : node->children)
    extent = std::max(extent, maxLargeOperatorExtent(child.get()));
  return extent;
}

const MathRenderNode* firstKind(const MathRenderNode* node, MathRenderKind kind) {
  if (node == nullptr) return nullptr;
  if (node->kind == kind) return node;
  for (const auto& child : node->children)
    if (const MathRenderNode* found = firstKind(child.get(), kind)) return found;
  return nullptr;
}

bool containsNode(const MathRenderNode* root, const MathRenderNode* target) {
  if (!root || !target) return false;
  if (root == target) return true;
  for (const auto& child : root->children)
    if (containsNode(child.get(), target)) return true;
  return false;
}

const MathRenderNode* enclosingKind(const MathRenderNode* node,
                                    const MathRenderNode* target,
                                    MathRenderKind kind) {
  if (!node || !containsNode(node, target)) return nullptr;
  if (node->kind == kind) return node;
  for (const auto& child : node->children)
    if (const auto* enclosing = enclosingKind(child.get(), target, kind))
      return enclosing;
  return nullptr;
}

const MathRenderNode* singleSymbol(const MathRenderNode* node) {
  if (node == nullptr || symbolCount(node) != 1) return nullptr;
  if (node->kind == MathRenderKind::Symbol) return node;
  for (const auto& child : node->children) {
    const MathRenderNode* candidate = singleSymbol(child.get());
    if (candidate != nullptr) return candidate;
  }
  return nullptr;
}

const MathRenderNode* wrappedTextModeRun(const MathRenderNode* node) {
  const MathRenderNode* current = node;
  while (current) {
    if (current->textModeRun) return current;
    if (current->semanticKind != MathSemanticKind::None)
      return nullptr;
    if ((current->kind != MathRenderKind::Span &&
         current->kind != MathRenderKind::VList))
      return nullptr;
    const MathRenderNode* visibleChild = nullptr;
    for (const auto& child : current->children) {
      if (!child || (symbolCount(child.get()) == 0 &&
                     child->semanticKind == MathSemanticKind::None))
        continue;
      if (visibleChild) return nullptr;
      visibleChild = child.get();
    }
    current = visibleChild;
  }
  return nullptr;
}

bool containsTextModeRun(const MathRenderNode* node) {
  if (!node) return false;
  if (node->textModeRun) return true;
  return std::any_of(
      node->children.cbegin(), node->children.cend(),
      [](const std::unique_ptr<MathRenderNode>& child) {
        return containsTextModeRun(child.get());
      });
}

bool collectTextModeRun(const MathRenderNode* node, QString* text,
                        const MathRenderNode** firstSymbol) {
  if (!node || !text || node->phantom) return true;
  if (node->kind == MathRenderKind::Span && node->children.empty() &&
      !node->text.isEmpty() &&
      std::all_of(node->text.cbegin(), node->text.cend(),
                  [](QChar character) {
                    return character.isSpace() ||
                           character.category() == QChar::Other_Format;
                  })) {
    *text += node->text;
    return true;
  }
  if (node->kind == MathRenderKind::Symbol) {
    if (node->text.isEmpty()) return true;
    const bool shapingOnly = std::all_of(
        node->text.cbegin(), node->text.cend(), [](QChar character) {
          return character.isSpace() ||
                 character.category() == QChar::Other_Format;
        });
    if (qFuzzyIsNull(node->width) && !shapingOnly) return true;
    if (firstSymbol && !*firstSymbol) *firstSymbol = node;
    *text += node->text;
    return true;
  }
  if (node->semanticKind != MathSemanticKind::None ||
      node->kind != MathRenderKind::Span)
    return false;
  for (const auto& child : node->children)
    if (!collectTextModeRun(child.get(), text, firstSymbol)) return false;
  return true;
}

std::optional<MathShapedText> shapeTextModeRun(const QString& text,
                                               qreal fontScale) {
  return OpenTypeMathFont::instance().shapeMathMlTextWithFallback(
      text, mermaid::MermaidFontRegistry::familyStack(), fontScale);
}

qreal shapedTextCssHeight(const MathShapedText& shaped) {
  const qreal pixelAlignedInkHeight =
      std::ceil(std::max<qreal>(0.0, -shaped.inkBounds.top())) +
      std::ceil(std::max<qreal>(0.0, shaped.inkBounds.bottom()));
  if (shaped.fullEmLineBox)
    return std::max(std::round(shaped.fontPixelSize),
                    pixelAlignedInkHeight);
  if (shaped.formatControlledLineBox)
    return pixelAlignedInkHeight;
  // Visible STIX glyphs contribute the MathML row box; a whitespace-only
  // shaping run must not enlarge a fallback-only text line.
  if (shaped.compoundLineBox && shaped.hasVisibleMathRun)
    return std::max(std::ceil(shaped.inkBounds.height()),
                    std::round(shaped.fontPixelSize *
                               kMathTableRowHeightEm));
  return shaped.compoundLineBox
      ? std::ceil(shaped.inkBounds.height())
      : std::round(shaped.inkBounds.height());
}

void includeMiddleDelimiterLayoutExtents(const MathRenderNode* node,
                                         GlyphInkExtents* extents,
                                         bool root) {
  if (!node || !extents) return;
  if (!node->middleDelimiter.isEmpty()) {
    const QRectF bounds = middleDelimiterLayoutBounds(
        node->middleDelimiter, mathStyleScale(node));
    if (!bounds.isEmpty()) {
      extents->top = std::max(
          extents->top, std::max<qreal>(0.0, -bounds.top()));
      extents->bottom = std::max(
          extents->bottom, std::max<qreal>(0.0, bounds.bottom()));
    }
    return;
  }
  if (!root && node->kind == MathRenderKind::LeftRight) return;
  for (const auto& child : node->children)
    includeMiddleDelimiterLayoutExtents(child.get(), extents, false);
}

GlyphInkExtents shapedTextCssExtents(const MathShapedText& shaped) {
  const qreal inkTop = std::max<qreal>(0.0, -shaped.inkBounds.top());
  const qreal inkBottom = std::max<qreal>(0.0, shaped.inkBounds.bottom());
  const qreal allocation = shapedTextCssHeight(shaped);
  if (shaped.fullEmLineBox) {
    const qreal bottom = std::max(
        inkBottom, std::min(allocation, shaped.fontPixelSize * 0.1));
    return {std::max<qreal>(0.0, allocation - bottom), bottom};
  }
  return {inkTop + std::max<qreal>(
                       0.0, allocation - inkTop - inkBottom),
          inkBottom};
}

bool hasTextModeDescender(const MathRenderNode* node) {
  if (!node) return false;
  if (const MathRenderNode* textRun = wrappedTextModeRun(node)) {
    QString text;
    if (collectTextModeRun(textRun, &text)) {
      if (const auto shaped = shapeTextModeRun(
              text, mathStyleScale(textRun))) {
        const GlyphInkExtents extents = shapedTextCssExtents(*shaped);
        return extents.bottom > shaped->fontPixelSize * 0.1 + 0.01;
      }
    }
  }
  return std::any_of(
      node->children.cbegin(), node->children.cend(),
      [](const std::unique_ptr<MathRenderNode>& child) {
        return hasTextModeDescender(child.get());
      });
}

GlyphInkExtents glyphInkExtents(const MathRenderNode* node, qreal fontScale) {
  GlyphInkExtents result;
  if (node == nullptr) return result;
  if (!node->middleDelimiter.isEmpty()) {
    const QRectF ink = middleDelimiterLayoutBounds(
        node->middleDelimiter, fontScale);
    if (ink.isEmpty()) return result;
    result.top = std::max<qreal>(0.0, -ink.top());
    result.bottom = std::max<qreal>(0.0, ink.bottom());
    return result;
  }
  if (const MathRenderNode* textRun = wrappedTextModeRun(node)) {
    QString text;
    if (collectTextModeRun(textRun, &text)) {
      if (const auto shaped = shapeTextModeRun(
              text, mathStyleScale(textRun))) {
        return shapedTextCssExtents(*shaped);
      }
    }
  }
  if (node->semanticKind == MathSemanticKind::SupSub && node->children.size() == 2) {
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    const qreal baseScale = node->mathStyleSize >= 3
        ? constants.scriptScriptPercentScaleDown
        : node->mathStyleSize >= 2 ? constants.scriptPercentScaleDown : 1.0;
    const GlyphInkExtents base = glyphInkExtents(node->children.front().get(), baseScale);
    const MathRenderNode* scripts = node->children.back().get();
    GlyphInkExtents superscript;
    GlyphInkExtents subscript;
    if (node->scriptKind == MathScriptKind::SubSup && scripts->children.size() >= 2) {
      subscript = glyphInkExtents(scripts->children.front().get(), baseScale);
      superscript = glyphInkExtents(scripts->children.back().get(), baseScale);
    } else if (node->scriptKind == MathScriptKind::Subscript) {
      subscript = glyphInkExtents(scripts, baseScale);
    } else {
      superscript = glyphInkExtents(scripts, baseScale);
    }
    const qreal supShift = constants.superscriptShiftUp * baseScale;
    qreal subShift = constants.subscriptShiftDown * baseScale;
    if (node->scriptKind == MathScriptKind::SubSup) {
      const qreal gap = (supShift - superscript.bottom) -
                        (subscript.top - subShift);
      subShift += std::max<qreal>(
          0.0, constants.subSuperscriptGapMin * baseScale - gap);
    }
    result.top = node->scriptKind == MathScriptKind::Subscript
        ? base.top : std::max(base.top, supShift + superscript.top);
    result.bottom = node->scriptKind == MathScriptKind::Superscript
        ? base.bottom : std::max(base.bottom, subShift + subscript.bottom);
    return result;
  }
  if (node->semanticKind == MathSemanticKind::Fraction)
    return fractionMathMlExtents(
        node, OpenTypeMathFont::instance().pixelSize(), fontScale);
  if (node->semanticKind == MathSemanticKind::Radical) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    const MathFontConstants& constants = font.constants();
    const MathRenderNode* radicalBox = node->radicalIndex
        ? indexedRadicalBox(node) : node;
    const MathRenderNode* body = radicalBody(radicalBox);
    if (!body) return result;
    const qreal styleScale = mathStyleScale(node);
    const qreal radicalGap = constants.radicalVerticalGap * styleScale;
    const qreal radicalRule = constants.radicalRuleThickness * styleScale;
    const qreal radicalExtra = constants.radicalExtraAscender * styleScale;
    const GlyphInkExtents bodyInk = glyphInkExtents(body, styleScale);
    qreal bodyHeight = primarySemanticNode(body)
        ? cssNodeHeight(body, styleScale)
        : std::ceil(bodyInk.top + bodyInk.bottom);
    if (const auto glyph = nativeGlyphBox(singleSymbol(body)))
      bodyHeight = std::round(glyph->height * styleScale);
    else if (!primarySemanticNode(body) && symbolCount(body) > 1)
      bodyHeight += 1.0;
    const qreal target = std::max(bodyInk.top + bodyInk.bottom, bodyHeight) +
                         radicalGap + radicalRule;
    const auto variant = font.verticalVariant(
        QString(QChar(0x221A)), target / styleScale);
    if (!variant) return bodyInk;
    const qreal bodyAscent = std::min(bodyHeight, bodyInk.top);
    result.top = std::max(
        bodyAscent, bodyInk.top + radicalGap + radicalRule + radicalExtra);
    result.bottom = std::max(
        bodyHeight - bodyAscent,
        variant->extent * styleScale + radicalExtra - result.top);
    return result;
  }
  if (node->kind == MathRenderKind::Symbol && node->text.size() > 1 &&
      node->fontClass == QLatin1String("main")) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    if (const auto shaped = font.shapeMathMlText(
            node->text, mathStyleScale(node))) {
      result.top = std::max<qreal>(0.0, -shaped->inkBounds.top());
      result.bottom = std::max<qreal>(0.0, shaped->inkBounds.bottom());
      return result;
    }
  }
  if (node->kind == MathRenderKind::Symbol && node->text.size() == 1) {
    const QChar character = node->text.front();
    const bool italic = character.isLetter() &&
        (node->fontClass == QLatin1String("mathnormal") ||
         node->fontClass == QLatin1String("mathit"));
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    if (node->mathStyleSize >= 3)
      fontScale = font.constants().scriptScriptPercentScaleDown;
    else if (node->mathStyleSize >= 2)
      fontScale = font.constants().scriptPercentScaleDown;
    else if (!node->tightSpacing)
      fontScale = 1.0;
    const auto glyph = italic ? font.mathItalicGlyph(character) : font.glyph(node->text);
    if (glyph) {
      const auto pixelRound = [fontScale](qreal value) {
        return fontScale < 0.999 ? std::round(value * fontScale)
                                : std::ceil(value * fontScale);
      };
      const bool latinDescender = QStringLiteral("gjpqy").contains(
          character, Qt::CaseInsensitive);
      const bool nonLatinLetter = character.isLetter() &&
                                  character.script() != QChar::Script_Latin;
      const bool usesInkDescent = latinDescender || nonLatinLetter;
      result.top = fontScale < 0.999 && !usesInkDescent
          ? std::round(glyph->inkBounds.height() * fontScale)
          : pixelRound(std::max<qreal>(0.0, -glyph->inkBounds.top()));
      if (nonLatinLetter) {
        const qreal total = std::round(glyph->inkBounds.height() * fontScale);
        result.bottom = std::max<qreal>(0.0, total - result.top);
      } else if (latinDescender) {
        result.bottom = pixelRound(std::max<qreal>(0.0, glyph->inkBounds.bottom()));
      }
      return result;
    }
  }
  for (const auto& child : node->children) {
    const GlyphInkExtents extents = glyphInkExtents(child.get(), fontScale);
    result.top = std::max(result.top, extents.top);
    result.bottom = std::max(result.bottom, extents.bottom);
  }
  return result;
}

qreal nativeOperatorPadding(const MathRenderNode* symbol) {
  if (!symbol) return 0.0;
  const qreal styleScale = symbol->mathStyleSize >= 3
      ? OpenTypeMathFont::instance().constants().scriptScriptPercentScaleDown
      : symbol->mathStyleSize >= 2 || symbol->tightSpacing
          ? OpenTypeMathFont::instance().constants().scriptPercentScaleDown
          : 1.0;
  if (symbol->atomClass == QLatin1String("mbin") ||
      symbol->text == QLatin1String("+"))
    return 4.0 / 18.0 * OpenTypeMathFont::instance().pixelSize() *
           styleScale;
  if (symbol->atomClass == QLatin1String("mrel"))
    return 5.0 / 18.0 * OpenTypeMathFont::instance().pixelSize() *
           styleScale;
  if (symbol->atomClass == QLatin1String("mop"))
    return 3.0 / 18.0 * OpenTypeMathFont::instance().pixelSize() *
           styleScale;
  return 0.0;
}

std::optional<NativeGlyphBox> nativeGlyphBox(const MathRenderNode* symbol) {
  if (symbol == nullptr || symbol->text.size() != 1) return std::nullopt;
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const QChar character = symbol->text.front();
  const bool italicMathIdentifier = character.isLetter() &&
      (symbol->fontClass == QLatin1String("mathnormal") ||
       symbol->fontClass == QLatin1String("mathit"));
  const auto glyph = italicMathIdentifier ? font.mathItalicGlyph(character)
                                          : font.glyph(symbol->text);
  if (!glyph) return std::nullopt;
  const qreal styleScale = symbol->mathStyleSize >= 3
      ? font.constants().scriptScriptPercentScaleDown
      : symbol->mathStyleSize >= 2 || symbol->tightSpacing
          ? font.constants().scriptPercentScaleDown : 1.0;
  qreal width = glyph->advance * styleScale;
  if (symbol->atomClass == QLatin1String("mop")) {
    const auto variant = font.verticalVariant(
        symbol->text, font.constants().displayOperatorMinHeight);
    if (variant) width = variant->advance * styleScale;
  }
  width += 2.0 * nativeOperatorPadding(symbol);
  const bool binaryOperator = symbol->atomClass == QLatin1String("mbin") ||
                              symbol->text == QLatin1String("+");
  const bool usesInkDescent = (character.isLetter() &&
                               character.script() != QChar::Script_Latin) ||
      QStringLiteral("gjpqy").contains(character, Qt::CaseInsensitive);
  qreal height = std::ceil(std::max<qreal>(0.0, -glyph->inkBounds.top()) +
                           (usesInkDescent
                                ? std::max<qreal>(0.0, glyph->inkBounds.bottom())
                                : 0.0));
  if (binaryOperator) height += 2.0;
  return NativeGlyphBox{width, height};
}

bool mathMlOperatorIsStretchy(const QString& character) {
  return character != QString(QChar(0x2223)) &&
         character != QString(QChar(0x2225));
}

MathCssBoxRole roleFor(const MathRenderNode& node) {
  switch (node.semanticKind) {
    case MathSemanticKind::Fraction: return MathCssBoxRole::Fraction;
    case MathSemanticKind::Radical: return MathCssBoxRole::Radical;
    case MathSemanticKind::SupSub: return MathCssBoxRole::SupSub;
    case MathSemanticKind::Array: return MathCssBoxRole::Array;
    case MathSemanticKind::None: break;
  }
  return node.kind == MathRenderKind::Symbol ? MathCssBoxRole::Glyph
                                             : MathCssBoxRole::Row;
}

MathCssBox convertTree(const MathRenderNode& node, qreal scale) {
  MathCssBox box;
  box.role = roleFor(node);
  box.semanticKind = node.semanticKind;
  box.scriptKind = node.scriptKind;
  box.radicalIndex = node.radicalIndex;
  box.text = node.text;
  box.x = node.xOffset * scale;
  box.y = node.yOffset * scale;
  box.width = node.width * scale;
  box.height = (node.height + node.depth) * scale;
  box.advance = box.width;
  box.baseline = node.height * scale;
  box.inkTop = 0.0;
  box.inkBottom = box.height;
  box.children.reserve(static_cast<qsizetype>(node.children.size()));
  for (const auto& child : node.children)
    if (child) box.children.push_back(convertTree(*child, scale));
  return box;
}

}  // namespace muffin::math::mathml
