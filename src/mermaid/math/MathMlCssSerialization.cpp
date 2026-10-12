#include "mermaid/math/MathMlCssInternal.h"
#include <QJsonArray>
#include <cmath>

namespace muffin::math {
using mathml::kChromiumMathAxisOffsetPx;

MathCssPaintKind MathCssPaintOperation::kind() const {
  if (std::holds_alternative<MathCssGlyphRunGroupOperation>(payload))
    return MathCssPaintKind::GlyphRun;
  if (std::holds_alternative<MathCssRowOperation>(payload))
    return MathCssPaintKind::Row;
  if (std::holds_alternative<MathCssLeftRightOperation>(payload))
    return MathCssPaintKind::LeftRight;
  if (std::holds_alternative<MathCssMiddlePaintOperation>(payload))
    return MathCssPaintKind::MiddleDelimiter;
  if (std::holds_alternative<MathCssFractionPaint>(payload))
    return MathCssPaintKind::Fraction;
  if (std::holds_alternative<MathCssScriptOperation>(payload))
    return MathCssPaintKind::SupSub;
  if (std::holds_alternative<MathCssRadicalOperation>(payload))
    return MathCssPaintKind::Radical;
  if (std::holds_alternative<MathCssArrayOperation>(payload))
    return MathCssPaintKind::Array;
  return MathCssPaintKind::Accent;
}

MathSemanticKind MathCssPaintOperation::semanticKind() const {
  if (std::holds_alternative<MathCssFractionPaint>(payload))
    return MathSemanticKind::Fraction;
  if (std::holds_alternative<MathCssScriptOperation>(payload))
    return MathSemanticKind::SupSub;
  if (std::holds_alternative<MathCssRadicalOperation>(payload))
    return MathSemanticKind::Radical;
  if (std::holds_alternative<MathCssArrayOperation>(payload))
    return MathSemanticKind::Array;
  return MathSemanticKind::None;
}

QRectF MathCssPaintOperation::container() const {
  if (const auto* glyphRun =
          std::get_if<MathCssGlyphRunGroupOperation>(&payload))
    return glyphRun->container;
  if (const auto* row = std::get_if<MathCssRowOperation>(&payload))
    return row->container;
  if (const auto* leftRight =
          std::get_if<MathCssLeftRightOperation>(&payload))
    return leftRight->container;
  if (const auto* middle =
          std::get_if<MathCssMiddlePaintOperation>(&payload))
    return middle->container;
  if (const auto* fraction = std::get_if<MathCssFractionPaint>(&payload))
    return fraction->box.container;
  if (const auto* script = std::get_if<MathCssScriptOperation>(&payload))
    return script->container;
  if (const auto* radical = std::get_if<MathCssRadicalOperation>(&payload))
    return radical->container;
  if (const auto* array = std::get_if<MathCssArrayOperation>(&payload))
    return array->container;
  return std::get<MathCssAccentOperation>(payload).container;
}

qreal MathCssPaintOperation::lineAscent() const {
  if (const auto* glyphRun =
          std::get_if<MathCssGlyphRunGroupOperation>(&payload))
    return glyphRun->lineAscent;
  if (const auto* row = std::get_if<MathCssRowOperation>(&payload))
    return row->lineAscent;
  if (const auto* leftRight =
          std::get_if<MathCssLeftRightOperation>(&payload))
    return leftRight->lineAscent;
  if (const auto* middle =
          std::get_if<MathCssMiddlePaintOperation>(&payload))
    return middle->lineAscent;
  if (const auto* fraction = std::get_if<MathCssFractionPaint>(&payload))
    return fraction->lineAscent;
  if (const auto* script = std::get_if<MathCssScriptOperation>(&payload))
    return script->lineAscent;
  if (const auto* radical = std::get_if<MathCssRadicalOperation>(&payload))
    return radical->lineAscent;
  if (const auto* array = std::get_if<MathCssArrayOperation>(&payload))
    return array->lineAscent;
  return std::get<MathCssAccentOperation>(payload).lineAscent;
}

qreal MathCssPaintOperation::alignmentBaseline() const {
  qreal baseline = container().height() / 2.0 + kChromiumMathAxisOffsetPx;
  if (kind() == MathCssPaintKind::Fraction)
    baseline = std::round(baseline * 2.0) / 2.0;
  return baseline;
}

namespace {

qreal jsonNumber(qreal value) {
  return std::round(value * 1000000.0) / 1000000.0;
}

QJsonObject rectJson(QRectF rect) {
  return {{QStringLiteral("x"), jsonNumber(rect.x())},
          {QStringLiteral("y"), jsonNumber(rect.y())},
          {QStringLiteral("width"), jsonNumber(rect.width())},
          {QStringLiteral("height"), jsonNumber(rect.height())}};
}

QJsonObject pointJson(QPointF point) {
  return {{QStringLiteral("x"), jsonNumber(point.x())},
          {QStringLiteral("y"), jsonNumber(point.y())}};
}

QJsonArray glyphRunsJson(const QVector<MathCssGlyphRunOperation>& runs) {
  QJsonArray result;
  for (const MathCssGlyphRunOperation& run : runs) {
    QJsonArray glyphIndexes;
    for (quint32 glyphIndex : run.glyphIndexes)
      glyphIndexes.push_back(static_cast<qint64>(glyphIndex));
    QJsonArray positions;
    for (QPointF position : run.positions)
      positions.push_back(pointJson(position));
    result.push_back(QJsonObject{
        {QStringLiteral("text"), run.text},
        {QStringLiteral("fontFamily"), run.fontFamily},
        {QStringLiteral("fontClass"), run.fontClass},
        {QStringLiteral("atomClass"), run.atomClass},
        {QStringLiteral("glyphIndexes"), glyphIndexes},
        {QStringLiteral("positions"), positions},
        {QStringLiteral("baselineOrigin"), pointJson(run.baselineOrigin)},
        {QStringLiteral("advance"), jsonNumber(run.advance)},
        {QStringLiteral("fontScale"), jsonNumber(run.fontScale)},
        {QStringLiteral("inkBounds"), rectJson(run.inkBounds)},
        {QStringLiteral("clip"), rectJson(run.clip)}});
  }
  return result;
}

QJsonObject verticalGlyphJson(const MathCssVerticalGlyphOperation& glyph) {
  QJsonArray parts;
  for (const MathCssVerticalGlyphPart& part : glyph.parts) {
    parts.push_back(QJsonObject{
        {QStringLiteral("glyphIndex"), static_cast<qint64>(part.glyphIndex)},
        {QStringLiteral("position"), pointJson(part.position)},
        {QStringLiteral("inkBounds"), rectJson(part.inkBounds)},
        {QStringLiteral("offset"), jsonNumber(part.offset)},
        {QStringLiteral("fullAdvance"), jsonNumber(part.fullAdvance)},
        {QStringLiteral("connectorOverlap"),
         jsonNumber(part.connectorOverlap)},
        {QStringLiteral("extender"), part.extender}});
  }
  return {
      {QStringLiteral("kind"),
       glyph.kind == MathCssVerticalGlyphKind::Assembly
           ? QStringLiteral("assembly") : QStringLiteral("fixed")},
      {QStringLiteral("scalePolicy"),
       glyph.scalePolicy == MathCssVerticalScalePolicy::FitTargetExtent
           ? QStringLiteral("fitTargetExtent")
           : glyph.scalePolicy ==
                     MathCssVerticalScalePolicy::FitSelectedExtent
               ? QStringLiteral("fitSelectedExtent")
               : QStringLiteral("preserveVariantScale")},
      {QStringLiteral("clipToBlockExtent"), glyph.clipToBlockExtent},
      {QStringLiteral("target"), rectJson(glyph.target)},
      {QStringLiteral("character"), glyph.character},
      {QStringLiteral("baselineOrigin"), pointJson(glyph.baselineOrigin)},
      {QStringLiteral("selectionTarget"), jsonNumber(glyph.selectionTarget)},
      {QStringLiteral("realizedExtent"), jsonNumber(glyph.realizedExtent)},
      {QStringLiteral("advance"), jsonNumber(glyph.advance)},
      {QStringLiteral("italicCorrection"),
       jsonNumber(glyph.italicCorrection)},
      {QStringLiteral("inkBounds"), rectJson(glyph.inkBounds)},
      {QStringLiteral("fixedGlyphIndex"),
       static_cast<qint64>(glyph.fixedGlyphIndex)},
      {QStringLiteral("parts"), parts}};
}

QJsonObject fencePairJson(const MathCssFencePair& fences) {
  QJsonObject result{{QStringLiteral("left"), rectJson(fences.left)},
                     {QStringLiteral("right"), rectJson(fences.right)},
                     {QStringLiteral("leftCharacter"),
                      fences.leftCharacter},
                     {QStringLiteral("rightCharacter"),
                      fences.rightCharacter}};
  if (fences.leftGlyph)
    result.insert(QStringLiteral("leftGlyph"),
                  verticalGlyphJson(*fences.leftGlyph));
  if (fences.rightGlyph)
    result.insert(QStringLiteral("rightGlyph"),
                  verticalGlyphJson(*fences.rightGlyph));
  return result;
}

QString paintKindJsonName(MathCssPaintKind kind) {
  switch (kind) {
    case MathCssPaintKind::GlyphRun: return QStringLiteral("glyph-run");
    case MathCssPaintKind::Row: return QStringLiteral("row");
    case MathCssPaintKind::LeftRight: return QStringLiteral("left-right");
    case MathCssPaintKind::MiddleDelimiter:
      return QStringLiteral("middle-delimiter");
    case MathCssPaintKind::Fraction: return QStringLiteral("fraction");
    case MathCssPaintKind::Radical: return QStringLiteral("radical");
    case MathCssPaintKind::SupSub: return QStringLiteral("supsub");
    case MathCssPaintKind::Array: return QStringLiteral("array");
    case MathCssPaintKind::Accent: return QStringLiteral("accent");
  }
  return {};
}

}  // namespace

QJsonObject MathCssPaintOperation::toJson() const {
  QJsonObject result{
      {QStringLiteral("kind"), paintKindJsonName(kind())},
      {QStringLiteral("container"), rectJson(container())},
      {QStringLiteral("lineAscent"), jsonNumber(lineAscent())}};
  if (const auto* glyphRun =
          std::get_if<MathCssGlyphRunGroupOperation>(&payload)) {
    result.insert(QStringLiteral("runs"), glyphRunsJson(glyphRun->runs));
  } else if (const auto* row =
                 std::get_if<MathCssRowOperation>(&payload)) {
    result.insert(QStringLiteral("glyphRuns"), glyphRunsJson(row->glyphRuns));
  } else if (const auto* leftRight =
                 std::get_if<MathCssLeftRightOperation>(&payload)) {
    result.insert(QStringLiteral("body"), rectJson(leftRight->body));
    result.insert(QStringLiteral("leftDelimiter"),
                  rectJson(leftRight->leftDelimiter));
    result.insert(QStringLiteral("rightDelimiter"),
                  rectJson(leftRight->rightDelimiter));
    if (leftRight->leftDelimiterGlyph)
      result.insert(QStringLiteral("leftDelimiterGlyph"),
                    verticalGlyphJson(*leftRight->leftDelimiterGlyph));
    if (leftRight->rightDelimiterGlyph)
      result.insert(QStringLiteral("rightDelimiterGlyph"),
                    verticalGlyphJson(*leftRight->rightDelimiterGlyph));
    QJsonArray regions;
    for (const MathCssLeftRightBodyRegion& region : leftRight->bodyRegions) {
      regions.push_back(QJsonObject{
          {QStringLiteral("box"), rectJson(region.box)},
          {QStringLiteral("glyphRuns"), glyphRunsJson(region.glyphRuns)}});
    }
    result.insert(QStringLiteral("bodyRegions"), regions);
    QJsonArray middles;
    for (const MathCssMiddleDelimiterOperation& middle :
         leftRight->middleDelimiters) {
      QJsonObject value{{QStringLiteral("character"), middle.character},
                        {QStringLiteral("box"), rectJson(middle.box)}};
      if (middle.glyph)
        value.insert(QStringLiteral("glyph"),
                     verticalGlyphJson(*middle.glyph));
      middles.push_back(value);
    }
    result.insert(QStringLiteral("middleDelimiters"), middles);
  } else if (const auto* middle =
                 std::get_if<MathCssMiddlePaintOperation>(&payload)) {
    result.insert(QStringLiteral("character"), middle->character);
    result.insert(QStringLiteral("allocation"),
                  rectJson(middle->allocation));
    result.insert(QStringLiteral("glyphRun"),
                  glyphRunsJson({middle->glyphRun}).at(0));
    if (middle->glyph)
      result.insert(QStringLiteral("glyph"),
                    verticalGlyphJson(*middle->glyph));
  } else if (const auto* fraction = std::get_if<MathCssFractionPaint>(&payload)) {
    result.insert(QStringLiteral("fraction"), rectJson(fraction->box.fraction));
    result.insert(QStringLiteral("numerator"), rectJson(fraction->box.numerator));
    result.insert(QStringLiteral("denominator"), rectJson(fraction->box.denominator));
    result.insert(QStringLiteral("rule"), rectJson(fraction->box.rule));
    result.insert(QStringLiteral("hasRule"), fraction->box.hasRule);
    result.insert(QStringLiteral("numeratorGlyphRuns"),
                  glyphRunsJson(fraction->numeratorGlyphRuns));
    result.insert(QStringLiteral("denominatorGlyphRuns"),
                  glyphRunsJson(fraction->denominatorGlyphRuns));
    if (fraction->leftDelimiterGlyph)
      result.insert(QStringLiteral("leftDelimiterGlyph"),
                    verticalGlyphJson(*fraction->leftDelimiterGlyph));
    if (fraction->rightDelimiterGlyph)
      result.insert(QStringLiteral("rightDelimiterGlyph"),
                    verticalGlyphJson(*fraction->rightDelimiterGlyph));
  } else if (const auto* script =
                 std::get_if<MathCssScriptOperation>(&payload)) {
    result.insert(QStringLiteral("scriptKind"),
                  static_cast<int>(script->kind));
    if (script->limits) result.insert(QStringLiteral("limits"), true);
    result.insert(QStringLiteral("base"), rectJson(script->base));
    result.insert(QStringLiteral("superscript"), rectJson(script->superscript));
    result.insert(QStringLiteral("subscript"), rectJson(script->subscript));
    result.insert(QStringLiteral("baseGlyphRuns"),
                  glyphRunsJson(script->baseGlyphRuns));
    result.insert(QStringLiteral("superscriptGlyphRuns"),
                  glyphRunsJson(script->superscriptGlyphRuns));
    result.insert(QStringLiteral("subscriptGlyphRuns"),
                  glyphRunsJson(script->subscriptGlyphRuns));
    if (script->largeOperatorGlyph)
      result.insert(QStringLiteral("largeOperatorGlyph"),
                    verticalGlyphJson(*script->largeOperatorGlyph));
    if (script->fences)
      result.insert(QStringLiteral("fences"),
                    fencePairJson(*script->fences));
  } else if (const auto* radical =
                 std::get_if<MathCssRadicalOperation>(&payload)) {
    result.insert(QStringLiteral("body"), rectJson(radical->body));
    result.insert(QStringLiteral("degree"), rectJson(radical->degree));
    result.insert(QStringLiteral("glyph"),
                  rectJson(radical->radicalGlyph.target));
    result.insert(QStringLiteral("rule"),
                  rectJson(radical->radicalRule.target));
    result.insert(QStringLiteral("radicalGlyph"), QJsonObject{
        {QStringLiteral("character"), radical->radicalGlyph.character},
        {QStringLiteral("glyphIndex"),
         static_cast<qint64>(radical->radicalGlyph.glyphIndex)},
        {QStringLiteral("target"), rectJson(radical->radicalGlyph.target)},
        {QStringLiteral("inkBounds"),
         rectJson(radical->radicalGlyph.inkBounds)},
        {QStringLiteral("clip"), rectJson(radical->radicalGlyph.clip)},
        {QStringLiteral("fontScale"),
         jsonNumber(radical->radicalGlyph.fontScale)}});
    result.insert(QStringLiteral("bodyGlyphRuns"),
                  glyphRunsJson(radical->bodyGlyphRuns));
    result.insert(QStringLiteral("degreeGlyphRuns"),
                  glyphRunsJson(radical->degreeGlyphRuns));
    if (radical->fences)
      result.insert(QStringLiteral("fences"),
                    fencePairJson(*radical->fences));
  } else if (const auto* array =
                 std::get_if<MathCssArrayOperation>(&payload)) {
    result.insert(QStringLiteral("table"), rectJson(array->table));
    QJsonArray rows;
    for (QRectF rowRect : array->rows) rows.push_back(rectJson(rowRect));
    result.insert(QStringLiteral("rows"), rows);
    QJsonArray cells;
    for (const MathCssArrayCell& cell : array->cells) {
      cells.push_back(QJsonObject{
          {QStringLiteral("row"), cell.row},
          {QStringLiteral("column"), cell.column},
          {QStringLiteral("box"), rectJson(cell.box)},
          {QStringLiteral("content"), rectJson(cell.content)},
          {QStringLiteral("baseline"), jsonNumber(cell.baseline)},
          {QStringLiteral("glyphRuns"), glyphRunsJson(cell.glyphRuns)}});
    }
    result.insert(QStringLiteral("cells"), cells);
    result.insert(QStringLiteral("leftDelimiter"),
                  array->leftDelimiterCharacter);
    result.insert(QStringLiteral("rightDelimiter"),
                  array->rightDelimiterCharacter);
    if (array->leftDelimiterGlyph)
      result.insert(QStringLiteral("leftDelimiterGlyph"),
                    verticalGlyphJson(*array->leftDelimiterGlyph));
    if (array->rightDelimiterGlyph)
      result.insert(QStringLiteral("rightDelimiterGlyph"),
                    verticalGlyphJson(*array->rightDelimiterGlyph));
  } else if (const auto* accent =
                 std::get_if<MathCssAccentOperation>(&payload)) {
    result.insert(QStringLiteral("accentKind"),
                  static_cast<int>(accent->accentKind));
    result.insert(QStringLiteral("body"), rectJson(accent->box.body));
    result.insert(QStringLiteral("accent"), rectJson(accent->box.accent));
    result.insert(QStringLiteral("annotation"),
                  rectJson(accent->box.annotation));
    result.insert(QStringLiteral("annotationContent"),
                  rectJson(accent->annotationContent));
    result.insert(QStringLiteral("character"), accent->box.character);
    result.insert(QStringLiteral("over"), accent->box.over);
    result.insert(QStringLiteral("bodyGlyphRuns"),
                  glyphRunsJson(accent->bodyGlyphRuns));
    result.insert(QStringLiteral("annotationGlyphRuns"),
                  glyphRunsJson(accent->annotationGlyphRuns));
    const MathCssHorizontalGlyphOperation& glyph = accent->glyph;
    QJsonObject glyphJson{
        {QStringLiteral("kind"),
         glyph.kind == MathCssHorizontalGlyphKind::Assembly
             ? QStringLiteral("assembly")
             : glyph.kind == MathCssHorizontalGlyphKind::ShapedText
                 ? QStringLiteral("shaped-text")
                 : QStringLiteral("fixed")},
        {QStringLiteral("scalePolicy"),
         glyph.scalePolicy ==
                 MathCssHorizontalScalePolicy::PreserveVariantScale
             ? QStringLiteral("preserve-variant")
             : glyph.scalePolicy ==
                       MathCssHorizontalScalePolicy::StretchInkToPlacementExtent
                   ? QStringLiteral("stretch-ink-to-placement")
                   : QStringLiteral("stretch-to-target")},
        {QStringLiteral("target"), rectJson(glyph.target)},
        {QStringLiteral("character"), glyph.character},
        {QStringLiteral("selectionTarget"),
         jsonNumber(glyph.selectionTarget)},
        {QStringLiteral("fontScale"), jsonNumber(glyph.fontScale)},
        {QStringLiteral("realizedExtent"),
         jsonNumber(glyph.realizedExtent)},
        {QStringLiteral("italicCorrection"),
         jsonNumber(glyph.italicCorrection)},
        {QStringLiteral("paintOffset"), pointJson(glyph.paintOffset)},
        {QStringLiteral("inkBounds"), rectJson(glyph.inkBounds)},
        {QStringLiteral("fixedGlyphIndex"),
         static_cast<qint64>(glyph.fixedGlyphIndex)}};
    if (glyph.kind == MathCssHorizontalGlyphKind::ShapedText) {
      glyphJson.insert(QStringLiteral("text"), glyph.text);
      QJsonArray indexes;
      for (quint32 index : glyph.textGlyphIndexes)
        indexes.push_back(static_cast<qint64>(index));
      glyphJson.insert(QStringLiteral("glyphIndexes"), indexes);
      QJsonArray positions;
      for (QPointF position : glyph.textGlyphPositions)
        positions.push_back(pointJson(position));
      glyphJson.insert(QStringLiteral("glyphPositions"), positions);
    }
    QJsonArray parts;
    for (const MathCssHorizontalGlyphPart& part : glyph.parts) {
      parts.push_back(QJsonObject{
          {QStringLiteral("glyphIndex"),
           static_cast<qint64>(part.glyphIndex)},
          {QStringLiteral("inkBounds"), rectJson(part.inkBounds)},
          {QStringLiteral("offset"), jsonNumber(part.offset)},
          {QStringLiteral("fullAdvance"), jsonNumber(part.fullAdvance)},
          {QStringLiteral("connectorOverlap"),
           jsonNumber(part.connectorOverlap)},
          {QStringLiteral("extender"), part.extender}});
    }
    glyphJson.insert(QStringLiteral("parts"), parts);
    result.insert(QStringLiteral("glyph"), glyphJson);
  }
  QJsonArray childrenJson;
  for (const MathCssPaintOperation& child : children)
    childrenJson.push_back(child.toJson());
  result.insert(QStringLiteral("children"), childrenJson);
  return result;
}

QString mathMlPaintFailureCodeName(MathMlPaintFailureCode code) {
  switch (code) {
    case MathMlPaintFailureCode::InvalidLayout:
      return QStringLiteral("invalid-layout");
    case MathMlPaintFailureCode::InvalidFontSize:
      return QStringLiteral("invalid-font-size");
    case MathMlPaintFailureCode::RootGlyphRunsUnavailable:
      return QStringLiteral("root-glyph-runs-unavailable");
    case MathMlPaintFailureCode::ChildOperationUnavailable:
      return QStringLiteral("child-operation-unavailable");
    case MathMlPaintFailureCode::AlignedChildOperationUnavailable:
      return QStringLiteral("aligned-child-operation-unavailable");
    case MathMlPaintFailureCode::AccentOperationUnavailable:
      return QStringLiteral("accent-operation-unavailable");
    case MathMlPaintFailureCode::UnsupportedRootOperation:
      return QStringLiteral("unsupported-root-operation");
    case MathMlPaintFailureCode::FractionOperationUnavailable:
      return QStringLiteral("fraction-operation-unavailable");
    case MathMlPaintFailureCode::RootOperationUnavailable:
      return QStringLiteral("root-operation-unavailable");
  }
  return QStringLiteral("unknown");
}

QString formatMathMlPaintFailure(const MathMlPaintFailure& failure) {
  QString result = QStringLiteral("MathML operation build failed [%1]")
                       .arg(mathMlPaintFailureCodeName(failure.code));
  if (!failure.nodePath.isEmpty())
    result += QStringLiteral(" at %1").arg(failure.nodePath);
  if (!failure.expectedMathMlTag.isEmpty())
    result += QStringLiteral(" (expected <%1>)")
                  .arg(failure.expectedMathMlTag);
  return result;
}

MathMlPaintError::MathMlPaintError(MathMlPaintFailure failure)
    : std::runtime_error(
          formatMathMlPaintFailure(failure).toUtf8().constData()),
      failure_(std::move(failure)) {}

QJsonObject MathMlPaintFailure::toJson() const {
  const auto renderKindName = [](MathRenderKind value) {
    switch (value) {
      case MathRenderKind::Span: return "span";
      case MathRenderKind::Symbol: return "symbol";
      case MathRenderKind::Rule: return "rule";
      case MathRenderKind::Rect: return "rect";
      case MathRenderKind::Sqrt: return "sqrt";
      case MathRenderKind::SupSub: return "sup-sub";
      case MathRenderKind::Fraction: return "fraction";
      case MathRenderKind::Accent: return "accent";
      case MathRenderKind::Phantom: return "phantom";
      case MathRenderKind::Stretchy: return "stretchy";
      case MathRenderKind::LeftRight: return "left-right";
      case MathRenderKind::Array: return "array";
      case MathRenderKind::VList: return "vlist";
      case MathRenderKind::Error: return "error";
    }
    return "unknown";
  };
  const auto semanticKindName = [](MathSemanticKind value) {
    switch (value) {
      case MathSemanticKind::None: return "none";
      case MathSemanticKind::Fraction: return "fraction";
      case MathSemanticKind::Radical: return "radical";
      case MathSemanticKind::SupSub: return "sup-sub";
      case MathSemanticKind::Array: return "array";
    }
    return "unknown";
  };
  QJsonObject result{
      {QStringLiteral("code"), mathMlPaintFailureCodeName(code)},
      {QStringLiteral("renderKind"),
       QString::fromLatin1(renderKindName(renderKind))},
      {QStringLiteral("semanticKind"),
       QString::fromLatin1(semanticKindName(semanticKind))},
      {QStringLiteral("nodePath"), nodePath}};
  if (!expectedMathMlTag.isEmpty())
    result.insert(QStringLiteral("expectedMathMlTag"), expectedMathMlTag);
  return result;
}

}  // namespace muffin::math
