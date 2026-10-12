#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math::mathml {

bool hasPaintOperation(MathSemanticKind kind) {
  return kind == MathSemanticKind::Fraction ||
         kind == MathSemanticKind::Radical ||
         kind == MathSemanticKind::SupSub ||
         kind == MathSemanticKind::Array;
}

bool ownsAccentPaintOperation(const MathRenderNode* node) {
  return ownedHorizontalAccent(node) != nullptr;
}

bool ownsGenericPaintOperation(const MathRenderNode* node) {
  return node && ((!node->middleDelimiter.isEmpty()) ||
                  (node->kind == MathRenderKind::LeftRight &&
                   primarySemanticNode(node) == nullptr) ||
                  hasPaintOperation(node->semanticKind)) &&
         !ownsAccentPaintOperation(node);
}

bool ownsPaintOperation(const MathRenderNode* node) {
  return ownsAccentPaintOperation(node) || ownsGenericPaintOperation(node);
}

void collectImmediatePaintNodes(const MathRenderNode* node,
                                QVector<const MathRenderNode*>* operations) {
  if (!node || !operations) return;
  if (ownsPaintOperation(node)) {
    operations->push_back(node);
    return;
  }
  if (node->semanticKind != MathSemanticKind::None) return;
  for (const auto& child : node->children)
    collectImmediatePaintNodes(child.get(), operations);
}

std::optional<MathCssPaintOperation> buildPaintOperation(
    const MathRenderNode* operationNode, const MathRenderNode* containingNode,
    QRectF containingRect, qreal renderScale, qreal cssRootFontPixelSize,
    MathMlStyleContext style, bool topLevel, bool rootRowChild) {
  if (!operationNode || !containingNode) return std::nullopt;
  if (!operationNode->middleDelimiter.isEmpty())
    return buildMiddlePaintOperation(
        operationNode, containingNode, containingRect, renderScale);
  if (operationNode->kind == MathRenderKind::LeftRight)
    return buildLeftRightOperation(
        operationNode, containingNode, containingRect, renderScale,
        cssRootFontPixelSize, style);
  if (ownsAccentPaintOperation(operationNode))
    return buildAccentOperation(
        operationNode, containingNode, containingRect, renderScale,
        cssRootFontPixelSize, style, topLevel, rootRowChild);
  if (operationNode->semanticKind == MathSemanticKind::Fraction) {
    const bool ownsRegion = primarySemanticNode(containingNode) == operationNode;
    const qreal left = containingRect.left() + cssNodeOffset(
        containingNode, operationNode, renderScale).value_or(0.0);
    const qreal fractionHeight = topLevel
        ? fractionMathMlHeight(
              operationNode, cssRootFontPixelSize, renderScale)
        : nestedFractionCssHeight(
              operationNode, cssRootFontPixelSize, renderScale);
    qreal height = fractionHeight;
    qreal delimiterTarget = fractionHeight;
    if (operationNode->fractionHasBarLine) {
      delimiterTarget += operationNode->fractionLineThicknessEm >= 0.0
          ? operationNode->fractionLineThicknessEm * cssRootFontPixelSize *
                operationNode->fractionSizeMultiplier
          : OpenTypeMathFont::instance().constants().fractionRuleThickness *
                operationNode->fractionSizeMultiplier;
    }
    height = std::max(height, std::ceil(
        fractionDelimiterExtent(operationNode, delimiterTarget)));
    if (ownsRegion) height = containingRect.height();
    const qreal top = ownsRegion
        ? containingRect.top()
        : containingRect.top() + (containingRect.height() - height) / 2.0;
    return buildFractionOperation(
        operationNode, containingNode, left, top, height, renderScale,
        cssRootFontPixelSize, true, style);
  }
  if (operationNode->semanticKind == MathSemanticKind::Array)
    return buildArrayOperation(
        operationNode, containingNode, containingRect, renderScale,
        cssRootFontPixelSize, style);

  MathCssPaintOperation result;
  if (operationNode->semanticKind == MathSemanticKind::SupSub) {
    auto script = buildScriptOperation(
        operationNode, containingNode, containingRect, renderScale);
    if (!script) return std::nullopt;
    result.payload = std::move(*script);
    if (const MathRenderNode* leftRight = enclosingKind(
            containingNode, operationNode, MathRenderKind::LeftRight)) {
      auto& scriptResult = std::get<MathCssScriptOperation>(result.payload);
      MathCssFencePair fences;
      const qreal leftWidth = arrayDelimiterWidth(
          leftRight->leftDelimiter, containingRect.height());
      const qreal rightWidth = arrayDelimiterWidth(
          leftRight->rightDelimiter, containingRect.height());
      fences.left = QRectF(containingRect.left(), containingRect.top(),
                           leftWidth, containingRect.height());
      fences.right = QRectF(containingRect.right() - rightWidth,
                            containingRect.top(), rightWidth,
                            containingRect.height());
      fences.leftCharacter = mathDelimiterCharacter(
          leftRight->leftDelimiter);
      fences.rightCharacter = mathDelimiterCharacter(
          leftRight->rightDelimiter);
      fences.leftGlyph = buildInlineFenceGlyphOperation(
          fences.leftCharacter, fences.left);
      fences.rightGlyph = buildInlineFenceGlyphOperation(
          fences.rightCharacter, fences.right);
      scriptResult.fences = std::move(fences);
      const qreal absoluteBaseline = scriptResult.container.top() +
                                     scriptResult.lineAscent;
      scriptResult.container = containingRect;
      scriptResult.lineAscent = absoluteBaseline -
                                scriptResult.container.top();
      scriptResult.lineDescent = scriptResult.container.height() -
                                 scriptResult.lineAscent;
    }
  } else if (operationNode->semanticKind == MathSemanticKind::Radical) {
    auto radical = buildRadicalOperation(
        operationNode, containingNode, containingRect, renderScale, topLevel);
    if (!radical) return std::nullopt;
    result.payload = std::move(*radical);
  } else {
    return std::nullopt;
  }

  const auto appendRegion = [&](const MathRenderNode* regionNode,
                                QRectF regionRect,
                                MathMlStyleContext regionStyle) {
    if (!regionNode || regionRect.isEmpty()) return;
    QVector<const MathRenderNode*> nodes;
    collectImmediatePaintNodes(regionNode, &nodes);
    for (const MathRenderNode* node : nodes) {
      if (auto child = buildPaintOperation(
              node, regionNode, regionRect, renderScale,
              cssRootFontPixelSize, regionStyle))
        result.children.push_back(std::move(*child));
    }
  };
  if (auto* radical = std::get_if<MathCssRadicalOperation>(&result.payload);
      radical && topLevel && !operationNode->radicalIndex) {
    QVector<const MathRenderNode*> bodyOperations;
    collectImmediatePaintNodes(radical->bodyNode, &bodyOperations);
    qreal bodyBaseline = middleDelimiterMarker(radical->bodyNode)
        ? mathMlInlineAscent(style.fontScale())
        : cssRootFontPixelSize;
    if (bodyOperations.size() == 1 &&
        primarySemanticNode(radical->bodyNode) == bodyOperations.front()) {
      if (const auto probe = buildPaintOperation(
              bodyOperations.front(), radical->bodyNode, radical->body,
              renderScale, cssRootFontPixelSize, style)) {
        bodyBaseline = probe->alignmentBaseline();
        radical->body.moveTop(snapLayoutUnit(
            radical->container.top() + result.alignmentBaseline() -
            bodyBaseline));
        if (auto child = buildPaintOperation(
                bodyOperations.front(), radical->bodyNode, radical->body,
                renderScale, cssRootFontPixelSize, style))
          result.children.push_back(std::move(*child));
        return result;
      }
    }
    if (rootRowChild) {
      const auto rowInk = paintOutlineInkExtents(
          containingNode, style.fontScale(), true);
      const auto bodyInk = paintOutlineInkExtents(
          radical->bodyNode, style.fontScale(), true);
      if (rowInk && bodyInk) {
        const qreal rowBaseline = containingRect.center().y() +
            (rowInk->top - rowInk->bottom) / 2.0;
        const qreal bodyPaintBaseline = radical->body.center().y() +
            (bodyInk->top - bodyInk->bottom) / 2.0;
        radical->body.moveTop(snapLayoutUnit(
            radical->body.top() + rowBaseline - bodyPaintBaseline));
      }
    } else {
      radical->body.moveTop(snapLayoutUnit(
          radical->container.top() + result.alignmentBaseline() -
          bodyBaseline));
    }
  }
  if (auto* radical = std::get_if<MathCssRadicalOperation>(&result.payload)) {
    if (auto runs = buildGlyphRunOperations(
            radical->bodyNode, radical->body, style.fontScale(), true))
      radical->bodyGlyphRuns = std::move(*runs);
    const MathRenderNode* degreeSymbol = singleSymbol(radical->degreeNode);
    const MathRenderNode* degreePaintNode =
        primarySemanticNode(radical->degreeNode)
        ? nullptr : degreeSymbol ? degreeSymbol : radical->degreeNode;
    if (auto runs = buildGlyphRunOperations(
            degreePaintNode, radical->degree,
            OpenTypeMathFont::instance().constants()
                .scriptScriptPercentScaleDown,
            true))
      radical->degreeGlyphRuns = std::move(*runs);
  }
  if (auto* script = std::get_if<MathCssScriptOperation>(&result.payload)) {
    const std::optional<qreal> cssPositionScale =
        script->largeOperatorGlyph ? std::optional<qreal>(renderScale)
                                   : std::nullopt;
    if (!script->largeOperatorGlyph) {
      if (auto runs = buildGlyphRunOperations(
              script->baseNode, script->base, style.fontScale(), true))
        script->baseGlyphRuns = std::move(*runs);
    }
    if (auto runs = buildGlyphRunOperations(
            script->superscriptNode, script->superscript,
            style.scriptStyle().fontScale(), true, cssPositionScale))
      script->superscriptGlyphRuns = std::move(*runs);
    if (auto runs = buildGlyphRunOperations(
            script->subscriptNode, script->subscript,
            style.scriptStyle().fontScale(), true, cssPositionScale))
      script->subscriptGlyphRuns = std::move(*runs);
    if (script->largeOperatorGlyph) {
      script->lineAscent = script->base.center().y() -
                           script->container.top() +
                           OpenTypeMathFont::instance().constants().axisHeight;
      script->lineDescent = script->container.height() -
                            script->lineAscent;
    }
  }
  if (const auto* script = std::get_if<MathCssScriptOperation>(&result.payload)) {
    appendRegion(script->baseNode, script->base, style);
    appendRegion(script->superscriptNode, script->superscript,
                 style.scriptStyle());
    appendRegion(script->subscriptNode, script->subscript,
                 style.scriptStyle());
  } else if (const auto* radical =
                 std::get_if<MathCssRadicalOperation>(&result.payload)) {
    appendRegion(radical->bodyNode, radical->body, style);
    appendRegion(radical->degreeNode, radical->degree,
                 MathMlStyleContext{2});
  }
  return result;
}

}  // namespace muffin::math::mathml

namespace muffin::math {
using namespace mathml;

MathMlPaintOperationBuildResult buildMathMlPaintOperations(
    const MathLayoutResult& layout, qreal renderFontPixelSize,
    qreal cssRootFontPixelSize) {
  const auto fail = [&](MathMlPaintFailureCode code,
                        const MathRenderNode* node) {
    const MathRenderNode* failureNode = node ? node : layout.root.get();
    return MathMlPaintOperationBuildResult{
        std::nullopt,
        MathMlPaintFailure{code,
                           failureNode ? failureNode->kind
                                       : MathRenderKind::Error,
                           failureNode ? failureNode->semanticKind
                                       : MathSemanticKind::None,
                           nodePath(layout.root.get(), failureNode),
                           expectedMathMlTag(failureNode)}};
  };
  const auto succeed = [](MathCssPaintOperation operation) {
    return MathMlPaintOperationBuildResult{std::move(operation),
                                           std::nullopt};
  };
  if (!layout.valid())
    return fail(MathMlPaintFailureCode::InvalidLayout, layout.root.get());
  if (renderFontPixelSize <= 0.0 || cssRootFontPixelSize <= 0.0)
    return fail(MathMlPaintFailureCode::InvalidFontSize, layout.root.get());
  const qreal renderScale = cssRootFontPixelSize / renderFontPixelSize;
  const MathCssBox root = layoutMathMlCssBox(
      layout, renderFontPixelSize, cssRootFontPixelSize);
  QVector<const MathRenderNode*> rootOperations;
  collectImmediatePaintNodes(layout.root.get(), &rootOperations);
  if (rootOperations.isEmpty()) {
    const QRectF rootRect(0.0, 0.0, root.width, root.height);
    auto runs = buildGlyphRunOperations(layout.root.get(), rootRect, 1.0);
    if (!runs) {
      if (const MathRenderNode* accent =
              firstKind(layout.root.get(), MathRenderKind::Accent))
        return fail(MathMlPaintFailureCode::AccentOperationUnavailable,
                    accent);
      return fail(MathMlPaintFailureCode::RootGlyphRunsUnavailable,
                  layout.root.get());
    }
    MathCssPaintOperation operation;
    operation.payload = MathCssGlyphRunGroupOperation{
        rootRect, std::move(*runs), root.baseline};
    return succeed(std::move(operation));
  }

  const QRectF rootRect(0.0, 0.0, root.width, root.height);
  auto rootGlyphRuns = buildGlyphRunOperations(
      layout.root.get(), rootRect, 1.0, true);
  if (rootOperations.size() > 1 || rootGlyphRuns) {
    MathCssPaintOperation row;
    row.payload = MathCssRowOperation{
        rootRect, layout.root.get(),
        rootGlyphRuns ? std::move(*rootGlyphRuns)
                      : QVector<MathCssGlyphRunOperation>{},
        root.baseline};
    row.children.reserve(rootOperations.size());
    for (const MathRenderNode* childNode : rootOperations) {
      auto child = buildPaintOperation(
          childNode, layout.root.get(), rootRect, renderScale,
          cssRootFontPixelSize, {}, true, true);
      if (!child)
        return fail(MathMlPaintFailureCode::ChildOperationUnavailable,
                    childNode);
      row.children.push_back(std::move(*child));
    }
    auto& rowPayload = std::get<MathCssRowOperation>(row.payload);
    const bool containsLargeOperator = std::any_of(
        row.children.cbegin(), row.children.cend(),
        [](const MathCssPaintOperation& child) {
          const auto* script = std::get_if<MathCssScriptOperation>(
              &child.payload);
          return script && script->largeOperatorGlyph.has_value();
        });
    if (containsLargeOperator) {
      auto cssPositionedRuns = buildGlyphRunOperations(
          layout.root.get(), rootRect, 1.0, true, renderScale);
      if (!cssPositionedRuns)
        return fail(MathMlPaintFailureCode::RootGlyphRunsUnavailable,
                    layout.root.get());
      rowPayload.glyphRuns = std::move(*cssPositionedRuns);
    }
    const auto operationContentBaseline =
        [](const MathCssPaintOperation& operation) -> std::optional<qreal> {
      if (const auto* accent =
              std::get_if<MathCssAccentOperation>(&operation.payload);
          accent && !accent->bodyGlyphRuns.isEmpty())
        return accent->bodyGlyphRuns.front().baselineOrigin.y();
      if (const auto* radical =
              std::get_if<MathCssRadicalOperation>(&operation.payload);
          radical && !radical->bodyGlyphRuns.isEmpty())
        return radical->container.top() + radical->lineAscent;
      if (const auto* leftRight =
              std::get_if<MathCssLeftRightOperation>(&operation.payload)) {
        for (const MathCssLeftRightBodyRegion& region :
             leftRight->bodyRegions)
          if (!region.glyphRuns.isEmpty())
            return leftRight->container.top() + leftRight->lineAscent;
      }
      if (const auto* script =
              std::get_if<MathCssScriptOperation>(&operation.payload);
          script && !script->baseGlyphRuns.isEmpty())
        return script->container.top() + script->lineAscent;
      if (const auto* array =
              std::get_if<MathCssArrayOperation>(&operation.payload))
        return array->table.center().y() +
               OpenTypeMathFont::instance().constants().axisHeight;
      if (const auto* fraction =
              std::get_if<MathCssFractionPaint>(&operation.payload))
        return fraction->box.container.center().y() +
               OpenTypeMathFont::instance().constants().axisHeight;
      return std::nullopt;
    };
    const auto alignContentGlyphRuns = [](MathCssPaintOperation* operation,
                                          qreal baseline) {
      if (!operation) return;
      QVector<MathCssGlyphRunOperation>* runs = nullptr;
      if (auto* accent =
              std::get_if<MathCssAccentOperation>(&operation->payload))
        runs = &accent->bodyGlyphRuns;
      else if (auto* radical =
                   std::get_if<MathCssRadicalOperation>(&operation->payload))
        runs = &radical->bodyGlyphRuns;
      else if (auto* leftRight =
                   std::get_if<MathCssLeftRightOperation>(&operation->payload)) {
        const auto region = std::find_if(
            leftRight->bodyRegions.begin(), leftRight->bodyRegions.end(),
            [](const MathCssLeftRightBodyRegion& candidate) {
              return !candidate.glyphRuns.isEmpty();
            });
        if (region != leftRight->bodyRegions.end()) runs = &region->glyphRuns;
      } else if (auto* script =
                     std::get_if<MathCssScriptOperation>(&operation->payload))
        runs = &script->baseGlyphRuns;
      if (!runs || runs->isEmpty()) return;
      const qreal shift = baseline - runs->front().baselineOrigin.y();
      for (MathCssGlyphRunOperation& run : *runs) {
        run.baselineOrigin.ry() += shift;
        run.inkBounds.translate(0.0, shift);
      }
    };
    const auto* firstAccent = std::get_if<MathCssAccentOperation>(
        &row.children.front().payload);
    const bool rootOwnsBaseline = !rowPayload.glyphRuns.isEmpty() &&
        firstAccent && firstAccent->accentKind == MathAccentKind::Under;
    const qreal contentBaseline = snapLayoutUnit(rootOwnsBaseline
        ? rowPayload.glyphRuns.front().baselineOrigin.y()
        : operationContentBaseline(row.children.front()).value_or(
              rootRect.center().y() +
              OpenTypeMathFont::instance().constants().axisHeight));
    if (!rowPayload.glyphRuns.isEmpty()) {
      const qreal translation =
          contentBaseline - rowPayload.glyphRuns.front().baselineOrigin.y();
      for (MathCssGlyphRunOperation& run : rowPayload.glyphRuns) {
        run.baselineOrigin.ry() += translation;
        run.inkBounds.translate(0.0, translation);
      }
    }
    for (qsizetype index = 0; index < row.children.size(); ++index) {
      if (const auto childBaseline =
              operationContentBaseline(row.children.at(index))) {
        const qreal height = row.children.at(index).container().height();
        const qreal intrinsicBaseline =
            *childBaseline - row.children.at(index).container().top();
        QRectF alignedRect = rootRect;
        alignedRect.setTop(std::clamp(
            contentBaseline - intrinsicBaseline,
            rootRect.top(),
            std::max(rootRect.top(), rootRect.bottom() - height)));
        alignedRect.setHeight(height);
        auto aligned = buildPaintOperation(
            rootOperations.at(index), layout.root.get(), alignedRect,
            renderScale, cssRootFontPixelSize, {}, true, true);
        if (!aligned)
          return fail(
              MathMlPaintFailureCode::AlignedChildOperationUnavailable,
              rootOperations.at(index));
        row.children[index] = std::move(*aligned);
        alignContentGlyphRuns(&row.children[index], contentBaseline);
      }
    }
    return succeed(std::move(row));
  }
  const MathRenderNode* owner = rootOperations.front();
  if (ownsAccentPaintOperation(owner)) {
    auto operation = buildAccentOperation(
        owner, layout.root.get(),
        QRectF(0.0, 0.0, root.width, root.height), renderScale,
        cssRootFontPixelSize, {}, true);
    if (!operation)
      return fail(MathMlPaintFailureCode::AccentOperationUnavailable,
                  owner);
    return succeed(std::move(*operation));
  }
  if (!ownsGenericPaintOperation(owner))
    return fail(MathMlPaintFailureCode::UnsupportedRootOperation, owner);
  if (owner->semanticKind == MathSemanticKind::Fraction) {
    qreal offset = cssNodeOffset(
        layout.root.get(), owner, renderScale).value_or(0.0);
    if (const MathRenderNode* leftRight = enclosingKind(
            layout.root.get(), owner, MathRenderKind::LeftRight))
      offset = arrayDelimiterWidth(leftRight->leftDelimiter, root.height);
    auto operation = buildFractionOperation(
        owner, layout.root.get(), offset, 0.0, root.height, renderScale,
        cssRootFontPixelSize, false, {});
    if (!operation)
      return fail(MathMlPaintFailureCode::FractionOperationUnavailable,
                  owner);
    return succeed(std::move(*operation));
  }
  auto operation = buildPaintOperation(
      owner, layout.root.get(), QRectF(0.0, 0.0, root.width, root.height),
      renderScale, cssRootFontPixelSize, {}, true);
  if (!operation)
    return fail(MathMlPaintFailureCode::RootOperationUnavailable, owner);
  return succeed(std::move(*operation));
}

std::optional<MathCssFractionBox> layoutMathMlFractionBox(
    const MathLayoutResult& layout, qreal renderFontPixelSize,
    qreal cssRootFontPixelSize) {
  const auto operation = buildMathMlPaintOperations(
      layout, renderFontPixelSize, cssRootFontPixelSize).operation;
  if (!operation) return std::nullopt;
  const auto* fraction = std::get_if<MathCssFractionPaint>(
      &operation->payload);
  return fraction ? std::optional<MathCssFractionBox>{fraction->box}
                  : std::nullopt;
}

}  // namespace muffin::math
