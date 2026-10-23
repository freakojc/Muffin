#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math::mathml {

QString basicAccentCharacter(const MathRenderNode* accent) {
  if (!accent || accent->kind != MathRenderKind::Accent || accent->children.size() < 2)
    return {};
  if (accent->accentKind == MathAccentKind::Overline || accent->accentKind == MathAccentKind::Underline)
    return QString(QChar(0x203E));
  if (accent->accentKind != MathAccentKind::Over || !accent->accentCharacter.isEmpty())
    return {};
  const MathRenderNode* accentPart = accent->children.back().get();
  if (const MathRenderNode* symbol = singleSymbol(accentPart))
    return symbol->text;
  if (const MathRenderNode* stretchy = firstKind(accentPart, MathRenderKind::Stretchy);
      stretchy && stretchy->pathName == QLatin1String("vec"))
    return QString(QChar(0x20D7));
  return {};
}

bool isSupportedHorizontalAccent(const MathRenderNode* node) {
  if (!node || node->kind != MathRenderKind::Accent)
    return false;
  const bool brace = node->accentKind == MathAccentKind::UnderBrace || node->accentKind == MathAccentKind::OverBrace;
  const bool arrow = (node->accentKind == MathAccentKind::Under || node->accentKind == MathAccentKind::Over) &&
                     !node->accentCharacter.isEmpty();
  return brace || arrow || !basicAccentCharacter(node).isEmpty();
}

const MathRenderNode* ownedHorizontalAccent(const MathRenderNode* node) {
  if (isSupportedHorizontalAccent(node))
    return node;
  if (!node || node->semanticKind != MathSemanticKind::SupSub || node->kind != MathRenderKind::VList)
    return nullptr;
  const MathRenderNode* accent = firstKind(node, MathRenderKind::Accent);
  return isSupportedHorizontalAccent(accent) ? accent : nullptr;
}

std::optional<MathCssVerticalGlyphOperation> buildVerticalGlyphOperation(const QString& delimiter, QRectF target,
                                                                         bool normalizeDelimiter) {
  if (delimiter.isEmpty() || delimiter == QLatin1String(".") || target.isEmpty())
    return std::nullopt;

  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const QString character = normalizeDelimiter ? mathDelimiterCharacter(delimiter) : delimiter;
  if (character == QLatin1String("|") || character == QString(QChar(0x2223)) || character == QString(QChar(0x2225))) {
    const auto base = font.glyph(character);
    if (!base || base->glyphIndex == 0)
      return std::nullopt;
    MathCssVerticalGlyphOperation result;
    result.clipToBlockExtent = normalizeDelimiter;
    result.target = target;
    result.character = character;
    result.selectionTarget = target.height();
    result.kind = MathCssVerticalGlyphKind::FixedVariant;
    result.fixedGlyphIndex = base->glyphIndex;
    result.realizedExtent = base->inkBounds.height();
    result.advance = base->advance;
    result.inkBounds = font.rasterGlyphBounds(base->glyphIndex);
    if (result.inkBounds.isEmpty())
      return std::nullopt;
    result.baselineOrigin = target.center() - result.inkBounds.center();
    return result;
  }
  const auto largestFixed = font.verticalVariant(character, std::numeric_limits<qreal>::max());
  const auto assembly = largestFixed && target.height() > std::ceil(largestFixed->extent + 0.001)
                            ? font.verticalAssemblyParts(character, target.height())
                            : std::optional<MathGlyphAssembly>{};

  MathCssVerticalGlyphOperation result;
  result.target = target;
  result.character = character;
  result.selectionTarget = target.height();
  if (assembly && !assembly->parts.isEmpty()) {
    result.kind = MathCssVerticalGlyphKind::Assembly;
    result.realizedExtent = assembly->extent;
    result.advance = assembly->advance;
    result.italicCorrection = assembly->italicCorrection;
    result.baselineOrigin.setX(target.center().x());
    result.parts.reserve(assembly->parts.size());
    for (const MathGlyphAssemblyPart& part : assembly->parts) {
      const QRectF partInk = font.rasterGlyphBounds(part.glyphIndex);
      if (partInk.isEmpty())
        return std::nullopt;
      const QList<QPointF> advances = font.rasterFont().advancesForGlyphIndexes({part.glyphIndex});
      if (advances.isEmpty())
        return std::nullopt;
      const QPointF position(-advances.front().x() / 2.0, part.offset - partInk.top());
      const QRectF positionedInk = partInk.translated(position);
      result.parts.push_back({part.glyphIndex, position, positionedInk, part.offset, part.fullAdvance,
                              part.connectorOverlap, part.extender});
      result.inkBounds = result.inkBounds.isNull() ? positionedInk : result.inkBounds.united(positionedInk);
    }
    result.baselineOrigin.setY(target.top() - result.inkBounds.top());
    return result;
  }

  const auto fixed = font.verticalVariant(character, target.height());
  if (!fixed)
    return std::nullopt;
  result.kind = MathCssVerticalGlyphKind::FixedVariant;
  if (normalizeDelimiter) {
    result.scalePolicy = MathCssVerticalScalePolicy::FitSelectedExtent;
    result.clipToBlockExtent = true;
  }
  result.fixedGlyphIndex = fixed->glyphIndex;
  result.realizedExtent = fixed->extent;
  result.advance = fixed->advance;
  result.inkBounds = font.rasterGlyphBounds(fixed->glyphIndex);
  if (result.inkBounds.isEmpty())
    return std::nullopt;
  result.baselineOrigin = target.center() - result.inkBounds.center();
  return result;
}

std::optional<MathCssVerticalGlyphOperation> buildInlineFenceGlyphOperation(const QString& delimiter, QRectF target) {
  if (delimiter.isEmpty() || delimiter == QLatin1String(".") || target.isEmpty())
    return std::nullopt;
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const QString character = mathDelimiterCharacter(delimiter);
  const auto glyph = font.glyph(character);
  if (!glyph || glyph->glyphIndex == 0)
    return std::nullopt;
  const QRectF ink = font.rasterGlyphBounds(glyph->glyphIndex);
  if (ink.isEmpty())
    return std::nullopt;

  MathCssVerticalGlyphOperation result;
  result.kind = MathCssVerticalGlyphKind::FixedVariant;
  result.scalePolicy = MathCssVerticalScalePolicy::FitTargetExtent;
  result.clipToBlockExtent = true;
  result.target = target;
  result.character = character;
  result.selectionTarget = target.height();
  result.fixedGlyphIndex = glyph->glyphIndex;
  result.realizedExtent = ink.height();
  result.advance = glyph->advance;
  result.inkBounds = ink;
  const qreal inkTop = target.top() + kChromiumMathAxisOffsetPx - font.constants().axisHeight;
  result.baselineOrigin = QPointF(target.center().x() - ink.center().x(), inkTop - ink.top());
  return result;
}

std::optional<MathCssPaintOperation> buildMiddlePaintOperation(const MathRenderNode* marker,
                                                               const MathRenderNode* containingNode,
                                                               QRectF containingRect, qreal renderScale) {
  if (!marker || marker->middleDelimiter.isEmpty() || !containingNode)
    return std::nullopt;
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const QString character = mathMiddleDelimiterCharacter(marker->middleDelimiter);
  const auto glyph = font.glyph(character);
  if (!glyph || glyph->glyphIndex == 0)
    return std::nullopt;

  const qreal fontScale = mathStyleScale(marker);
  const QRectF layoutInk = middleDelimiterLayoutBounds(marker->middleDelimiter, fontScale);
  if (layoutInk.isEmpty())
    return std::nullopt;
  const QRectF rasterInk = font.rasterGlyphBounds(glyph->glyphIndex, fontScale);
  if (rasterInk.isEmpty())
    return std::nullopt;
  const qreal allocationWidth = cssNodeWidth(marker, renderScale);
  const qreal spacing = font.pixelSize() * kMathMlMiddleSpaceEm * fontScale;
  const qreal left = containingRect.left() + cssNodeOffset(containingNode, marker, renderScale).value_or(0.0);
  const qreal height = std::floor(layoutInk.height() + 0.001);
  const QRectF box(left + spacing, containingRect.top() + (containingRect.height() - height) / 2.0,
                   glyph->advance * fontScale, height);

  MathCssPaintOperation operation;
  operation.payload = MathCssMiddlePaintOperation{};
  auto& result = std::get<MathCssMiddlePaintOperation>(operation.payload);
  result.character = character;
  result.container = box;
  result.allocation = QRectF(left, box.top(), allocationWidth, height);
  result.lineAscent = std::max<qreal>(0.0, containingRect.height() - (symbolCount(containingNode) > 1 ? 1.0 : 0.0));
  result.glyphRun.text = character;
  result.glyphRun.glyphIndexes = {glyph->glyphIndex};
  result.glyphRun.positions = {QPointF()};
  result.glyphRun.fontScale = fontScale;
  result.glyphRun.advance = glyph->advance * fontScale;
  result.glyphRun.inkBounds = rasterInk.translated(box.topLeft() - rasterInk.topLeft());
  result.glyphRun.baselineOrigin = box.topLeft() - rasterInk.topLeft();
  result.glyphRun.clip = box;
  result.glyph = buildInlineFenceGlyphOperation(character, box);
  if (result.glyph) {
    result.glyph->character = character;
    result.glyph->fixedGlyphIndex = glyph->glyphIndex;
    result.glyph->advance = glyph->advance;
    result.glyph->inkBounds = font.rasterGlyphBounds(glyph->glyphIndex);
    result.glyph->realizedExtent = result.glyph->inkBounds.height();
    result.glyph->clipToBlockExtent = false;
  }
  return operation;
}

const MathRenderNode* leftRightBody(const MathRenderNode* leftRight) {
  if (!leftRight || leftRight->kind != MathRenderKind::LeftRight || leftRight->children.empty())
    return nullptr;
  const size_t bodyIndex = leftRight->leftDelimiter == QLatin1String(".") ? 0 : 1;
  return bodyIndex < leftRight->children.size() ? leftRight->children[bodyIndex].get() : nullptr;
}

std::optional<MathCssPaintOperation> buildLeftRightOperation(const MathRenderNode* leftRight,
                                                             const MathRenderNode* containingNode,
                                                             QRectF containingRect, qreal renderScale,
                                                             qreal cssRootFontPixelSize, MathMlStyleContext style) {
  const MathRenderNode* bodyNode = leftRightBody(leftRight);
  if (!bodyNode)
    return std::nullopt;

  const qreal width = cssNodeWidth(leftRight, renderScale);
  const bool participatesInRootRow = symbolCount(containingNode) > symbolCount(leftRight);
  const qreal height =
      participatesInRootRow ? std::ceil(cssNodeHeight(leftRight, renderScale)) : containingRect.height();
  const qreal left = containingRect.left() + cssNodeOffset(containingNode, leftRight, renderScale).value_or(0.0);
  const qreal top = containingRect.top() + (containingRect.height() - height) / 2.0;
  const qreal leftWidth = arrayDelimiterWidth(leftRight->leftDelimiter, height);
  const qreal rightWidth = arrayDelimiterWidth(leftRight->rightDelimiter, height);
  const qreal bodyWidth = middleDelimiterMarker(bodyNode) ? leftRightBodyCssWidth(bodyNode, renderScale)
                                                          : cssNodeWidth(bodyNode, renderScale);
  const qreal bodyHeight = std::min(height, cssNodeHeight(bodyNode, renderScale));

  MathCssPaintOperation operation;
  operation.payload = MathCssLeftRightOperation{};
  auto& result = std::get<MathCssLeftRightOperation>(operation.payload);
  result.container = QRectF(left, top, width, height);
  result.body = QRectF(left + leftWidth, top + (height - bodyHeight) / 2.0, bodyWidth, bodyHeight);
  result.leftDelimiter = QRectF(left, top, leftWidth, height);
  result.rightDelimiter = QRectF(result.body.right(), top, rightWidth, height);
  result.leftDelimiterCharacter = mathDelimiterCharacter(leftRight->leftDelimiter);
  result.rightDelimiterCharacter = mathDelimiterCharacter(leftRight->rightDelimiter);
  result.leftDelimiterGlyph = buildVerticalGlyphOperation(result.leftDelimiterCharacter, result.leftDelimiter);
  result.rightDelimiterGlyph = buildVerticalGlyphOperation(result.rightDelimiterCharacter, result.rightDelimiter);
  result.lineAscent = height / 2.0 + kChromiumMathAxisOffsetPx;

  const auto appendRegion = [&](const MathRenderNode* node, QRectF box) {
    if (!node || box.isEmpty())
      return;
    MathCssLeftRightBodyRegion region;
    region.box = box;
    region.node = node;
    if (auto runs = buildGlyphRunOperations(node, box, style.fontScale(), true))
      region.glyphRuns = std::move(*runs);
    result.bodyRegions.push_back(std::move(region));

    QVector<const MathRenderNode*> nodes;
    collectImmediatePaintNodes(node, &nodes);
    for (const MathRenderNode* childNode : nodes) {
      if (auto child = buildPaintOperation(childNode, node, box, renderScale, cssRootFontPixelSize, style))
        operation.children.push_back(std::move(*child));
    }
  };

  if (bodyNode->kind == MathRenderKind::Span) {
    qreal middleLineHeight = 0.0;
    for (const auto& child : bodyNode->children) {
      const MathRenderNode* marker = middleDelimiterMarker(child.get());
      if (!marker)
        continue;
      const QRectF layoutBounds = middleDelimiterLayoutBounds(marker->middleDelimiter, mathStyleScale(marker));
      if (layoutBounds.isEmpty())
        continue;
      middleLineHeight = std::max(middleLineHeight, layoutBounds.height());
    }
    qreal cursor = result.body.left();
    for (const auto& child : bodyNode->children) {
      if (!child)
        continue;
      const MathRenderNode* middleMarker = middleDelimiterMarker(child.get());
      const qreal childWidth =
          middleMarker ? leftRightBodyCssWidth(child.get(), renderScale) : cssNodeWidth(child.get(), renderScale);
      const bool nestedLeftRight = child->kind == MathRenderKind::LeftRight;
      const QRectF childBox(cursor, nestedLeftRight ? result.container.top() : result.body.top(), childWidth,
                            nestedLeftRight ? result.container.height() : result.body.height());
      if (middleMarker) {
        MathCssMiddleDelimiterOperation middle;
        middle.character = mathMiddleDelimiterCharacter(middleMarker->middleDelimiter);
        const QRectF layoutBounds =
            middleDelimiterLayoutBounds(middleMarker->middleDelimiter, mathStyleScale(middleMarker));
        const qreal middleHeight = !layoutBounds.isEmpty() ? layoutBounds.height() : result.container.height();
        const qreal middleSpace = OpenTypeMathFont::instance().pixelSize() * kMathMlMiddleSpaceEm;
        middle.box =
            QRectF(cursor + middleSpace, result.container.top() + (result.container.height() - middleLineHeight) / 2.0,
                   std::max<qreal>(0.0, childWidth - 2.0 * middleSpace), middleHeight);
        middle.glyph = buildVerticalGlyphOperation(middle.character, middle.box, false);
        result.middleDelimiters.push_back(std::move(middle));
      } else {
        appendRegion(child.get(), childBox);
      }
      cursor += childWidth;
    }
  } else {
    appendRegion(bodyNode, result.body);
  }
  return operation;
}

std::optional<MathCssPaintOperation> buildAccentOperation(const MathRenderNode* owner,
                                                          const MathRenderNode* containingNode, QRectF containingRect,
                                                          qreal renderScale, qreal cssRootFontPixelSize,
                                                          MathMlStyleContext style, bool topLevel, bool rootRowChild) {
  const MathRenderNode* accent = ownedHorizontalAccent(owner);
  if (!accent || accent->children.size() < 2)
    return std::nullopt;

  MathLayoutResult probe;
  probe.root = cloneNode(*owner);
  probe.naturalSize = QSizeF(owner->width, owner->height + owner->depth);
  probe.size = probe.naturalSize;
  probe.baseline = owner->height;
  const qreal renderFontPixelSize = cssRootFontPixelSize / renderScale;
  auto localBox = layoutMathMlAccentBox(probe, renderFontPixelSize, cssRootFontPixelSize);
  if (!localBox)
    return std::nullopt;
  MathCssBox localRoot = layoutMathMlCssBox(probe, renderFontPixelSize, cssRootFontPixelSize);

  QRectF localBounds;
  for (const QRectF& component : {localBox->body, localBox->accent, localBox->annotation}) {
    if (component.isEmpty())
      continue;
    localBounds = localBounds.isNull() ? component : localBounds.united(component);
  }
  if (!localBounds.isNull()) {
    const QPointF normalize(-localBounds.left(), 0.0);
    localBox->body.translate(normalize);
    localBox->accent.translate(normalize);
    localBox->annotation.translate(normalize);
    localRoot.width = localBounds.width();
  }

  const bool brace =
      accent->accentKind == MathAccentKind::UnderBrace || accent->accentKind == MathAccentKind::OverBrace;
  if (!topLevel && brace) {
    const qreal nestedAccentHeight =
        std::floor(localBox->accent.height() * OpenTypeMathFont::instance().constants().scriptPercentScaleDown);
    const qreal reduction = localBox->accent.height() - nestedAccentHeight;
    localBox->accent.setHeight(nestedAccentHeight);
    if (localBox->over) {
      localBox->body.translate(0.0, -reduction);
    } else {
      localBox->annotation.translate(0.0, -reduction);
    }
    localRoot.height -= reduction;
  }
  if (rootRowChild && accent->accentKind == MathAccentKind::OverBrace) {
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    const qreal allocationHeight = std::round(constants.overbarVerticalGap + 2.0 * constants.overbarRuleThickness);
    const qreal operatorHeight = std::round(constants.overbarVerticalGap);
    const qreal rowCollapse = std::max<qreal>(0.0, allocationHeight - operatorHeight);
    localBox->body.translate(0.0, -rowCollapse);
    localRoot.height -= rowCollapse;
  }

  const qreal left = containingRect.left() + cssNodeOffset(containingNode, owner, renderScale).value_or(0.0);
  qreal localHeight = std::max({localBox->body.bottom(), localBox->accent.bottom(), localBox->annotation.bottom()});
  if (accent->accentKind == MathAccentKind::Under)
    localHeight += OpenTypeMathFont::instance().constants().underbarExtraDescender;
  const qreal top =
      topLevel ? containingRect.top() : containingRect.top() + (containingRect.height() - localHeight) / 2.0;
  const QPointF translation(left, top);

  const bool bodyIsLast =
      accent->accentKind == MathAccentKind::Under || accent->accentKind == MathAccentKind::OverBrace;
  const MathRenderNode* body = bodyIsLast ? accent->children.back().get() : accent->children.front().get();
  const MathRenderNode* annotation = nullptr;
  if (owner->semanticKind == MathSemanticKind::SupSub) {
    for (const auto& child : owner->children) {
      if (child && !containsNode(child.get(), accent)) {
        annotation = child.get();
        break;
      }
    }
  }

  MathCssPaintOperation operation;
  operation.payload = MathCssAccentOperation{};
  MathCssAccentOperation& result = std::get<MathCssAccentOperation>(operation.payload);
  result.accentKind = accent->accentKind;
  result.box = *localBox;
  result.box.body.translate(translation);
  result.box.accent.translate(translation);
  result.box.annotation.translate(translation);
  result.container = QRectF(left, top, localRoot.width, localHeight);
  result.bodyNode = body;
  result.annotationNode = annotation;
  if (auto runs = buildGlyphRunOperations(body, result.box.body, style.fontScale(), true)) {
    // Brace bodies use a CSS ink line box whose extra operator leading is
    // allocated above the baseline. Keep raster bounds for painting, but
    // place them from the bundled-font layout descent so CoreText hinting
    // cannot move the whole body vertically.
    if (brace && !containsTextModeRun(body) && !runs->isEmpty()) {
      const GlyphInkExtents layoutInk = glyphInkExtents(body, style.fontScale());
      const qreal baseline = result.box.body.bottom() - std::min(result.box.body.height(), layoutInk.bottom);
      const qreal baselineShift = baseline - runs->front().baselineOrigin.y();
      for (MathCssGlyphRunOperation& run : *runs) {
        run.baselineOrigin.ry() += baselineShift;
        run.inkBounds.translate(0.0, baselineShift);
      }
    }
    result.bodyGlyphRuns = std::move(*runs);
  }
  if (annotation && !result.box.annotation.isEmpty()) {
    const qreal annotationWidth = cssNodeWidth(annotation, renderScale);
    const MathRenderNode* annotationSemantic = primarySemanticNode(annotation);
    const bool annotationOwnsCssLayout = annotationSemantic != nullptr || containsTextModeRun(annotation);
    const MathFontConstants& constants = OpenTypeMathFont::instance().constants();
    qreal annotationHeight =
        annotationOwnsCssLayout ? cssNodeHeight(annotation, renderScale)
                                : std::round(cssNodeHeight(annotation, renderScale) * constants.scriptPercentScaleDown);
    if (annotationSemantic && annotationSemantic->semanticKind == MathSemanticKind::Fraction &&
        annotationSemantic->fractionHasBarLine)
      annotationHeight += constants.fractionRuleThickness;
    const qreal annotationTop =
        annotationOwnsCssLayout && result.box.over ? result.container.top() + constants.overbarExtraAscender
        : result.box.over                          ? result.box.annotation.bottom() - annotationHeight
                          : result.box.annotation.bottom() - constants.underbarExtraDescender - annotationHeight;
    result.annotationContent = QRectF(result.box.annotation.center().x() - annotationWidth / 2.0, annotationTop,
                                      annotationWidth, annotationHeight);
    if (auto runs =
            buildGlyphRunOperations(annotation, result.annotationContent, style.scriptStyle().fontScale(), true))
      result.annotationGlyphRuns = std::move(*runs);
  }
  result.lineAscent = result.container.height() / 2.0 + kChromiumMathAxisOffsetPx;

  const auto appendRegion = [&](const MathRenderNode* regionNode, QRectF regionRect, MathMlStyleContext regionStyle) {
    if (!regionNode || regionRect.isEmpty())
      return;
    QVector<const MathRenderNode*> nodes;
    collectImmediatePaintNodes(regionNode, &nodes);
    for (const MathRenderNode* nested : nodes) {
      if (auto child =
              buildPaintOperation(nested, regionNode, regionRect, renderScale, cssRootFontPixelSize, regionStyle))
        operation.children.push_back(std::move(*child));
    }
  };
  appendRegion(body, result.box.body, style);
  appendRegion(annotation, result.annotationContent, style.scriptStyle());

  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  MathCssHorizontalGlyphOperation& glyph = result.glyph;
  glyph.target = result.box.accent;
  glyph.character = result.box.character;
  const bool basicAccent = !basicAccentCharacter(accent).isEmpty();
  const bool cappedAssembly = result.box.character == QString(QChar(0x21D2));
  const bool operatorAssembly = result.box.character == QString(QChar(0x2190)) || cappedAssembly;
  glyph.selectionTarget =
      accent->accentUsesNaturalWidth || basicAccent ? result.box.accent.width() : result.box.body.width();
  glyph.fontScale = result.box.fontScale * style.fontScale();
  const MathRenderNode* bodySemantic = primarySemanticNode(body);
  const bool scaleVariantSelection = accent->accentKind == MathAccentKind::UnderBrace && !bodySemantic;
  qreal stretchTarget = glyph.selectionTarget * (scaleVariantSelection ? glyph.fontScale : 1.0);
  if (cappedAssembly)
    stretchTarget = std::min(stretchTarget, std::floor(3.0 * result.box.accent.width()));
  glyph.placementExtent = cappedAssembly ? stretchTarget : glyph.selectionTarget;
  if (rootRowChild && accent->accentKind == MathAccentKind::Under) {
    glyph.paintOffset.setY(std::round(2.0 * font.constants().underbarRuleThickness));
  }
  glyph.scalePolicy = accent->accentUsesNaturalWidth || !result.bodyGlyphRuns.isEmpty() || bodySemantic
                          ? MathCssHorizontalScalePolicy::PreserveVariantScale
                          : MathCssHorizontalScalePolicy::StretchToTarget;
  if (result.box.character == QString(QChar(0x23E0)) || result.box.character == QString(QChar(0x23E1)))
    glyph.scalePolicy = MathCssHorizontalScalePolicy::StretchInkToPlacementExtent;
  const auto natural = accent->accentUsesNaturalWidth || basicAccent ? font.glyph(result.box.character)
                                                                     : std::optional<MathGlyphMetrics>{};
  const HorizontalAccentSelection selection =
      natural ? HorizontalAccentSelection{}
              : selectHorizontalAccent(result.box.character, stretchTarget, brace || operatorAssembly);
  if (natural) {
    glyph.kind = MathCssHorizontalGlyphKind::FixedVariant;
    glyph.fixedGlyphIndex = natural->glyphIndex;
    glyph.realizedExtent = natural->advance;
    glyph.inkBounds = font.rasterGlyphBounds(natural->glyphIndex, glyph.fontScale);
  } else if (selection.text) {
    glyph.kind = MathCssHorizontalGlyphKind::ShapedText;
    glyph.text = result.box.character;
    glyph.textGlyphIndexes = selection.text->glyphIndexes;
    glyph.textGlyphPositions = selection.text->positions;
    glyph.realizedExtent = selection.text->advance;
    glyph.inkBounds = selection.text->inkBounds;
  } else if (selection.fixed) {
    glyph.kind = MathCssHorizontalGlyphKind::FixedVariant;
    glyph.fixedGlyphIndex = selection.fixed->glyphIndex;
    glyph.realizedExtent = selection.fixed->extent;
    glyph.inkBounds = font.rasterGlyphBounds(selection.fixed->glyphIndex, glyph.fontScale);
  } else if (selection.assembly) {
    glyph.kind = MathCssHorizontalGlyphKind::Assembly;
    glyph.realizedExtent = selection.assembly->extent * glyph.fontScale;
    glyph.italicCorrection = selection.assembly->italicCorrection * glyph.fontScale;
    glyph.parts.reserve(selection.assembly->parts.size());
    const QTransform outlineScale = QTransform::fromScale(glyph.fontScale, glyph.fontScale);
    for (const MathGlyphAssemblyPart& part : selection.assembly->parts) {
      QRectF partInk = outlineScale.mapRect(font.glyphPath(part.glyphIndex).boundingRect());
      glyph.parts.push_back({part.glyphIndex, partInk, part.offset * glyph.fontScale,
                             part.fullAdvance * glyph.fontScale, part.connectorOverlap * glyph.fontScale,
                             part.extender});
      partInk.translate(part.offset * glyph.fontScale, 0.0);
      glyph.inkBounds = glyph.inkBounds.isNull() ? partInk : glyph.inkBounds.united(partInk);
    }
  } else {
    return std::nullopt;
  }
  return operation;
}

} // namespace muffin::math::mathml
