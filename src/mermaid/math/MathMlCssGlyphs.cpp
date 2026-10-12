#include "mermaid/math/MathMlCssInternal.h"
#include "mermaid/MermaidFontRegistry.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace muffin::math::mathml {

bool findPaintOrigin(const MathRenderNode* node,
                     const MathRenderNode* target, QPointF origin,
                     QPointF* result) {
  if (!node || !target || !result) return false;
  if (node == target) {
    *result = origin;
    return true;
  }
  const auto descend = [&](const MathRenderNode* child,
                           QPointF childOrigin) {
    return findPaintOrigin(child, target, childOrigin, result);
  };
  if (node->kind == MathRenderKind::Span) {
    qreal x = origin.x();
    for (const auto& child : node->children) {
      const QPointF childOrigin(
          x + child->xOffset,
          origin.y() + child->yOffset + child->shift);
      if (descend(child.get(), childOrigin)) return true;
      x += child->width;
    }
    return false;
  }
  if (node->kind == MathRenderKind::SupSub) {
    for (size_t index = 0; index < node->children.size(); ++index) {
      const auto& child = node->children[index];
      const QPointF childOrigin(
          origin.x() + child->xOffset,
          origin.y() + child->yOffset +
              (index == 0 ? 0.0 : child->shift));
      if (descend(child.get(), childOrigin)) return true;
    }
    return false;
  }
  if (node->kind == MathRenderKind::Fraction &&
      node->children.size() >= 2) {
    for (size_t index : {size_t{0}, node->children.size() - 1}) {
      const auto& child = node->children[index];
      const QPointF childOrigin(
          origin.x() + (node->width - child->width) / 2.0,
          origin.y() + child->shift);
      if (descend(child.get(), childOrigin)) return true;
    }
    if (node->children.size() >= 3 &&
        descend(node->children[1].get(), origin))
      return true;
    return false;
  }
  for (const auto& child : node->children) {
    const QPointF childOrigin(
        origin.x() + child->xOffset,
        origin.y() + child->yOffset + child->shift);
    if (descend(child.get(), childOrigin)) return true;
  }
  return false;
}

bool findCssPaintX(const MathRenderNode* node,
                   const MathRenderNode* target, qreal origin,
                   qreal* result) {
  if (!node || !target || !result) return false;
  if (node == target) {
    *result = origin;
    return true;
  }
  if (node->kind == MathRenderKind::Span) {
    qreal x = origin;
    for (const auto& child : node->children) {
      if (findCssPaintX(child.get(), target, x + child->xOffset, result))
        return true;
      x += cssNodeWidth(child.get(), 1.0);
    }
    return false;
  }
  for (const auto& child : node->children)
    if (findCssPaintX(child.get(), target, origin + child->xOffset, result))
      return true;
  return false;
}

bool collectGlyphRunSymbols(const MathRenderNode* node,
                            QVector<const MathRenderNode*>* symbols,
                            bool skipOwnedOperations) {
  if (!node || !symbols || node->phantom) return true;
  if (skipOwnedOperations && ownsPaintOperation(node)) return true;
  if (!node->middleDelimiter.isEmpty()) return skipOwnedOperations;
  if (node->textModeRun) {
    symbols->push_back(node);
    return true;
  }
  if (node->kind == MathRenderKind::Symbol ||
      node->kind == MathRenderKind::Error) {
    if (node->text.isEmpty()) return true;
    symbols->push_back(node);
    return true;
  }
  if (node->semanticKind != MathSemanticKind::None)
    return skipOwnedOperations;
  const bool zeroAdvanceOverlay =
      node->kind == MathRenderKind::Accent &&
      node->accentKind == MathAccentKind::None &&
      qFuzzyIsNull(node->width);
  if (node->kind != MathRenderKind::Span &&
      node->kind != MathRenderKind::VList && !zeroAdvanceOverlay)
    return false;
  for (const auto& child : node->children)
    if (!collectGlyphRunSymbols(child.get(), symbols, skipOwnedOperations))
      return false;
  return true;
}

std::optional<GlyphInkExtents> paintOutlineInkExtents(
    const MathRenderNode* node, qreal fontScale,
    bool allowPartialOwnership) {
  if (!node || fontScale <= 0.0) return std::nullopt;
  QVector<const MathRenderNode*> symbols;
  bool collected = collectGlyphRunSymbols(node, &symbols);
  if (!collected && allowPartialOwnership) {
    symbols.clear();
    collected = collectGlyphRunSymbols(node, &symbols, true);
  }
  if (!collected || symbols.isEmpty()) return std::nullopt;

  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  GlyphInkExtents result;
  bool hasVisibleInk = false;
  for (const MathRenderNode* symbol : symbols) {
    QRectF ink;
    if (const MathRenderNode* textRun = wrappedTextModeRun(symbol)) {
      QString text;
      if (!collectTextModeRun(textRun, &text) || text.isEmpty())
        return std::nullopt;
      const auto shaped = shapeTextModeRun(text, fontScale);
      if (!shaped) return std::nullopt;
      ink = shaped->inkBounds;
    } else if (symbol->text.size() == 1) {
      const QChar character = symbol->text.front();
      const bool italic = character.isLetter() &&
          (symbol->fontClass == QLatin1String("mathnormal") ||
           symbol->fontClass == QLatin1String("mathit"));
      const auto glyph = italic ? font.mathItalicGlyph(character)
                                : font.glyph(symbol->text);
      if (!glyph) return std::nullopt;
      ink = QRectF(glyph->inkBounds.x() * fontScale,
                   glyph->inkBounds.y() * fontScale,
                   glyph->inkBounds.width() * fontScale,
                   glyph->inkBounds.height() * fontScale);
    } else {
      const auto shaped = font.shapeMathMlText(symbol->text, fontScale);
      if (!shaped) return std::nullopt;
      ink = shaped->inkBounds;
    }
    if (ink.isEmpty()) continue;
    hasVisibleInk = true;
    result.top = std::max(result.top, std::max<qreal>(0.0, -ink.top()));
    result.bottom = std::max(
        result.bottom, std::max<qreal>(0.0, ink.bottom()));
  }
  return hasVisibleInk ? std::optional<GlyphInkExtents>{result}
                       : std::nullopt;
}

std::optional<QVector<MathCssGlyphRunOperation>> buildGlyphRunOperations(
    const MathRenderNode* node, QRectF target, qreal fontScale,
    bool allowPartialOwnership,
    std::optional<qreal> cssPositionScale) {
  if (!node || target.isEmpty() || node->width <= 0.0) return std::nullopt;
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  QVector<const MathRenderNode*> symbols;
  bool collected = collectGlyphRunSymbols(node, &symbols);
  if (!collected && allowPartialOwnership) {
    symbols.clear();
    collected = collectGlyphRunSymbols(node, &symbols, true);
  }
  if (!collected || symbols.isEmpty())
    return std::nullopt;

  QVector<MathCssGlyphRunOperation> runs;
  runs.reserve(symbols.size());
  const qreal horizontalScale = target.width() / node->width;
  const bool hasTextModeSource = std::any_of(
      symbols.cbegin(), symbols.cend(), [](const MathRenderNode* symbol) {
        return wrappedTextModeRun(symbol) != nullptr;
      });
  qreal inkTop = std::numeric_limits<qreal>::max();
  qreal inkBottom = std::numeric_limits<qreal>::lowest();
  for (const MathRenderNode* symbol : symbols) {
    QPointF sourceOrigin;
    if (!findPaintOrigin(node, symbol, QPointF(0.0, node->height),
                         &sourceOrigin)) {
      return std::nullopt;
    }
    if (hasTextModeSource) {
      qreal cssX = 0.0;
      if (!findCssPaintX(node, symbol, 0.0, &cssX)) return std::nullopt;
      sourceOrigin.setX(cssX);
    }
    if (cssPositionScale) {
      const auto cssX = cssNodeOffset(node, symbol, *cssPositionScale);
      if (!cssX) return std::nullopt;
      sourceOrigin.setX(*cssX + nativeOperatorPadding(symbol));
    }
    if (singleSymbol(node) == symbol) sourceOrigin.setX(0.0);
    if (const MathRenderNode* textRun = wrappedTextModeRun(symbol)) {
      QString text;
      const MathRenderNode* firstSymbol = nullptr;
      if (!collectTextModeRun(textRun, &text, &firstSymbol) ||
          text.isEmpty() || !firstSymbol)
        return std::nullopt;
      const auto shaped = shapeTextModeRun(text, fontScale);
      if (!shaped) return std::nullopt;
      for (const MathShapedTextRun& shapedRun : shaped->runs) {
        MathCssGlyphRunOperation run;
        run.text = shapedRun.text;
        run.fontFamily = shapedRun.familyName;
        run.fontClass = firstSymbol->fontClass;
        run.atomClass = textRun->atomClass;
        run.rawFont = shapedRun.rawFont;
        run.glyphIndexes = shapedRun.glyphIndexes;
        run.positions = shapedRun.positions;
        run.advance = shapedRun.advance;
        run.fontScale = fontScale;
        run.baselineOrigin.setX(
            target.left() + sourceOrigin.x() *
                (hasTextModeSource || cssPositionScale ? 1.0
                                                       : horizontalScale));
        run.inkBounds = shapedRun.inkBounds;
        inkTop = std::min(inkTop, run.inkBounds.top());
        inkBottom = std::max(inkBottom, run.inkBounds.bottom());
        run.clip = target;
        runs.push_back(std::move(run));
      }
      continue;
    }
    MathCssGlyphRunOperation run;
    run.text = symbol->text;
    run.fontClass = symbol->fontClass;
    run.atomClass = symbol->atomClass;
    run.fontScale = fontScale;
    run.baselineOrigin = QPointF(
        target.left() + sourceOrigin.x() *
            (hasTextModeSource || cssPositionScale ? 1.0
                                                   : horizontalScale),
        0.0);
    if (symbol->text.size() == 1) {
      const QChar character = symbol->text.front();
      const bool italic = character.isLetter() &&
          (symbol->fontClass == QLatin1String("mathnormal") ||
           symbol->fontClass == QLatin1String("mathit"));
      const auto glyph = italic ? font.mathItalicGlyph(character)
                                : font.glyph(symbol->text);
      if (glyph) {
        run.glyphIndexes.push_back(glyph->glyphIndex);
        run.positions.push_back(QPointF());
        run.advance = glyph->advance * run.fontScale;
        run.inkBounds = font.rasterGlyphBounds(
            glyph->glyphIndex, run.fontScale);
      } else {
        QRawFont rawFont = QRawFont::fromFont(symbol->font);
        rawFont.setPixelSize(font.pixelSize() * run.fontScale);
        const QList<quint32> indexes =
            rawFont.glyphIndexesForString(symbol->text);
        if (!rawFont.isValid() || indexes.size() != 1 ||
            indexes.front() == 0)
          return std::nullopt;
        run.rawFont = rawFont;
        run.fontFamily = rawFont.familyName();
        run.glyphIndexes.push_back(indexes.front());
        run.positions.push_back(QPointF());
        const QList<QPointF> advances =
            rawFont.advancesForGlyphIndexes(indexes);
        run.advance = advances.isEmpty() ? 0.0 : advances.front().x();
        run.inkBounds = rawFont.boundingRect(indexes.front());
      }
    } else {
      const auto shaped = font.shapeMathMlText(symbol->text, run.fontScale);
      if (!shaped) return std::nullopt;
      run.glyphIndexes = shaped->glyphIndexes;
      run.positions = shaped->positions;
      run.advance = shaped->advance;
      run.inkBounds = shaped->inkBounds;
    }
    inkTop = std::min(inkTop, run.inkBounds.top());
    inkBottom = std::max(inkBottom, run.inkBounds.bottom());
    run.clip = target;
    runs.push_back(run);
  }
  const qreal baseline = target.center().y() - (inkTop + inkBottom) / 2.0;
  for (MathCssGlyphRunOperation& run : runs) {
    run.baselineOrigin.setY(baseline);
    run.inkBounds.translate(run.baselineOrigin);
  }
  QVector<MathCssGlyphRunOperation> merged;
  merged.reserve(runs.size());
  for (MathCssGlyphRunOperation& run : runs) {
    if (!merged.isEmpty()) {
      MathCssGlyphRunOperation& previous = merged.back();
      const qreal offset = run.baselineOrigin.x() -
                           previous.baselineOrigin.x();
      const bool contiguous =
          qFuzzyCompare(previous.fontScale, run.fontScale) &&
          previous.fontFamily == run.fontFamily &&
          previous.fontClass == run.fontClass &&
          previous.atomClass == run.atomClass &&
          previous.clip == run.clip &&
          std::abs(offset - previous.advance) <= 1.0;
      if (contiguous) {
        previous.text += run.text;
        for (qsizetype index = 0; index < run.glyphIndexes.size(); ++index) {
          previous.glyphIndexes.push_back(run.glyphIndexes.at(index));
          previous.positions.push_back(
              run.positions.at(index) + QPointF(offset, 0.0));
        }
        previous.advance = std::max(
            previous.advance, offset + run.advance);
        previous.inkBounds = previous.inkBounds.united(run.inkBounds);
        continue;
      }
    }
    merged.push_back(std::move(run));
  }
  return merged;
}

GlyphInkExtents preciseTokenInkExtents(const MathRenderNode* node,
                                       qreal fontScale) {
  const MathRenderNode* symbol = singleSymbol(node);
  if (!symbol || symbol->text.size() != 1)
    return glyphInkExtents(node, fontScale);
  const QChar character = symbol->text.front();
  const bool italic = character.isLetter() &&
      (symbol->fontClass == QLatin1String("mathnormal") ||
       symbol->fontClass == QLatin1String("mathit"));
  const OpenTypeMathFont& font = OpenTypeMathFont::instance();
  const auto glyph = italic ? font.mathItalicGlyph(character)
                            : font.glyph(symbol->text);
  if (!glyph) return glyphInkExtents(node, fontScale);
  return {std::max<qreal>(0.0, -glyph->inkBounds.top()) * fontScale,
          std::max<qreal>(0.0, glyph->inkBounds.bottom()) * fontScale};
}

CssTokenLineMetrics cssTokenLineMetrics(const MathRenderNode* node,
                                        qreal renderScale,
                                        qreal fontScale) {
  CssTokenLineMetrics result;
  if (const MathRenderNode* textRun = wrappedTextModeRun(node)) {
    QString text;
    if (collectTextModeRun(textRun, &text)) {
      if (const auto shaped = shapeTextModeRun(text, fontScale)) {
        const GlyphInkExtents outline = shapedTextCssExtents(*shaped);
        const qreal allocation = shapedTextCssHeight(*shaped);
        const qreal descent = std::min(outline.bottom, allocation);
        result.ink = {allocation - descent, descent};
        result.ascent = result.ink.top;
        result.descent = result.ink.bottom;
        return result;
      }
    }
  }
  result.ink = preciseTokenInkExtents(node, fontScale);
  if (const MathRenderNode* symbol = singleSymbol(node);
      symbol && symbol->atomClass == QLatin1String("mop")) {
    const OpenTypeMathFont& font = OpenTypeMathFont::instance();
    if (const auto variant = font.verticalVariant(
            symbol->text, font.constants().displayOperatorMinHeight)) {
      const QRectF ink = font.rasterGlyphBounds(
          variant->glyphIndex, fontScale);
      result.ink = {std::max<qreal>(0.0, -ink.top()),
                    std::max<qreal>(0.0, ink.bottom())};
      const qreal height = largeOperatorAllocationHeight(
          symbol->text, *variant, fontScale);
      result.ascent = std::min(height, std::round(result.ink.top));
      result.descent = std::max<qreal>(0.0, height - result.ascent);
      return result;
    }
  }
  if (const MathRenderNode* symbol = singleSymbol(node);
      symbol && !primarySemanticNode(node)) {
    const qreal height = std::round(result.ink.top + result.ink.bottom);
    result.ascent = std::min(height, std::round(result.ink.top));
    result.descent = std::max<qreal>(0.0, height - result.ascent);
    if (fontScale >= 0.999) {
      if (const auto glyph = nativeGlyphBox(symbol)) {
        const qreal glyphHeight = std::round(glyph->height);
        result.descent += std::max<qreal>(0.0, glyphHeight - result.height());
      }
    }
    return result;
  }
  const qreal height = cssNodeHeight(node, renderScale);
  result.ascent = std::min(height, std::round(result.ink.top));
  result.descent = std::max<qreal>(0.0, height - result.ascent);
  return result;
}

}  // namespace muffin::math::mathml
