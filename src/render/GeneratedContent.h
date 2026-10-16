#pragma once

#include <QRegularExpression>
#include <algorithm>

#include "render/LayoutBox.h"
#include "theme/CssContent.h"
#include "theme/CssThemeMapper.h"
#include "theme/CssValueParser.h"
#include "theme/RenderTheme.h"

namespace muffin {

inline quint64 generatedContentFingerprint(const std::optional<PseudoElementRule>& rule) {
  return rule && rule->computed ? qHashMulti(size_t(0),rule->computed->fingerprint(),rule->content) : 0;
}

// A single projection of a live computed pseudo style serves normal flow and
// positioned fragments. Containing dimensions are supplied by layout, never paint.
struct GeneratedContentStyle {
  PseudoElementRule rule;
  ThemeElementStyle style;
  ThemeElementBoxStyle used;
  QFont font;
  QString text;
  qreal width = 0, height = 0;
  bool icon = false, atomic = false, block = false;
};

inline QSizeF constrainedGeneratedSize(const GeneratedContentStyle& value, QSizeF containing, QSizeF intrinsic, qreal zoom) {
  if (!value.rule.computed) return intrinsic;
  const auto& style = *value.rule.computed;
  const auto insets = LayoutBox::insets(value.used);
  const auto axis = [&](const char* dimension,const char* minimum,const char* maximum,const char* start,const char* end,
                        qreal basis,qreal size,qreal padding,qreal margins) {
    const auto used = [&](const CssLengthPercentage& length) { return length.px*zoom+length.fraction*basis; };
    const auto length = style.length(QLatin1String(dimension));
    const auto a = style.length(QLatin1String(start)), b = style.length(QLatin1String(end));
    const qreal extra = value.used.borderBox ? 0 : padding;
    if (length.status == CssLengthStatus::Valid) size = used(length)+extra;
    else if (value.rule.absolute && a.status == CssLengthStatus::Valid && b.status == CssLengthStatus::Valid)
      size = basis-used(a)-used(b)-margins;
    const auto max = style.length(QLatin1String(maximum)), min = style.length(QLatin1String(minimum));
    if (max.status == CssLengthStatus::Valid) size = qMin(size,used(max)+extra);
    if (min.status == CssLengthStatus::Valid) size = qMax(size,used(min)+extra);
    return qMax(padding,size);
  };
  return {axis("width","min-width","max-width","left","right",containing.width(),intrinsic.width(),insets.left()+insets.right(),value.used.margin.left()+value.used.margin.right()),
          axis("height","min-height","max-height","top","bottom",containing.height(),intrinsic.height(),insets.top()+insets.bottom(),value.used.margin.top()+value.used.margin.bottom())};
}

inline std::optional<GeneratedContentStyle> generatedContentStyle(const RenderTheme& theme, const PseudoElementRule& rule,
                                                                  const QFont& fallback, QSizeF containing, QString text = {}) {
  if (!rule.present) return {};
  if (rule.computed) {
    const auto content = rule.computed->resolvedValue("content").trimmed().toLower();
    if (content == "none" || content == "normal" || rule.computed->resolvedValue("display") == "none") return {};
  }
  GeneratedContentStyle value;
  value.rule = rule;
  value.icon = !rule.svgData.isEmpty();
  if (text.isEmpty() && !rule.content.isEmpty()) {
    if (!rule.contentTokens.empty()) {
      const auto& tokens = rule.contentTokens;
      if (std::all_of(tokens.begin(), tokens.end(), [](const auto& token) { return token.kind == ContentToken::Kind::Literal; }))
        for (const auto& token : tokens) text += token.text;
    } else
      text = rule.content;
  }
  value.text = std::move(text);
  value.atomic = value.icon || value.text.isEmpty();
  value.font = fallback;
  if (rule.computed) {
    value.style = CssThemeMapper::projectComputedStyle(rule.host + "::" + rule.pseudo, *rule.computed);
    value.font = theme.fontForStyle(value.style, fallback);
    value.block = rule.computed->resolvedValue("display") == "block";
  } else {
    value.style.paint.color=rule.color;value.style.paint.opacity=rule.opacity;
    value.style.paint.backgroundColor=rule.backgroundColor;
  }
  value.used = theme.usedBoxForStyle(value.style, containing.width());
  const qreal zoom = theme.zoomPercent() / 100.;
  const qreal em = value.font.pointSizeF() * 96. / 72.;
  const auto length = [&](const char* name, qreal basis, qreal fallbackValue) {
    const auto parsed = rule.computed ? rule.computed->length(QLatin1String(name)) : CssLengthPercentage{};
    return parsed.status == CssLengthStatus::Valid ? parsed.px * zoom + parsed.fraction * basis : fallbackValue;
  };
  const auto inset = LayoutBox::insets(value.used);
  const auto extraW = value.used.borderBox ? 0 : inset.left() + inset.right();
  const auto extraH = value.used.borderBox ? 0 : inset.top() + inset.bottom();
  const qreal autoWidth = value.block ? containing.width() : value.icon ? em : 0;
  value.width = qMax<qreal>(0, length("width", containing.width(), rule.size.width() > 0 ? rule.size.width() * zoom : autoWidth) + extraW);
  value.height = qMax<qreal>(0, length("height", containing.height() >= 0 ? containing.height() : em,
                                       rule.size.height() > 0 ? rule.size.height() * zoom
                                       : value.icon           ? em
                                                              : 0) +
                                    extraH);
  const auto size = constrainedGeneratedSize(value,containing,{value.width,value.height},zoom);
  value.width = size.width(); value.height = size.height();
  return value;
}

inline QRectF positionGeneratedContent(const GeneratedContentStyle& value, const QRectF& containing, QRectF box, qreal zoom,
                                       bool translate = true) {
  if (!value.rule.computed) return box;
  box.setSize(constrainedGeneratedSize(value,containing.size(),box.size(),zoom));
  const auto& style = *value.rule.computed;
  const auto used = [&](const char* name, qreal basis) {
    const auto v = style.length(QLatin1String(name));
    return v.px * zoom + v.fraction * basis;
  };
  const auto valid = [&](const char* name) { return style.length(QLatin1String(name)).status == CssLengthStatus::Valid; };
  if (valid("left"))
    box.moveLeft(containing.left() + used("left", containing.width()) + value.used.margin.left());
  else if (valid("right"))
    box.moveRight(containing.right() - used("right", containing.width()) - value.used.margin.right());
  if (valid("top"))
    box.moveTop(containing.top() + used("top", containing.height()) + value.used.margin.top());
  else if (valid("bottom"))
    box.moveBottom(containing.bottom() - used("bottom", containing.height()) - value.used.margin.bottom());
  if (!translate) return box;
  const CssLengthContext context{style.fontSizePx * style.textScale, style.rootFontSizePx * style.textScale, style.fontSizePx * .5,
                                 style.fontSizePx * .5, style.viewportPx};
  static const QRegularExpression translations(QStringLiteral(R"(translate(x|y)?\(([^()]*(?:\([^()]*\)[^()]*)*)\))"));
  auto matches = translations.globalMatch(style.resolvedValue("transform").toLower());
  while (matches.hasNext()) {
    const auto match = matches.next();
    auto args = match.captured(2);
    args.replace(',', ' ');
    const auto parts = splitTopLevelSpaces(args);
    if (parts.isEmpty()) continue;
    const auto translated = [&](const QString& text, qreal basis) {
      const auto length = parseCssLengthPercentage(QStringView(text), context);
      return length.status == CssLengthStatus::Valid ? length.px * zoom + length.fraction * basis : 0;
    };
    if (match.captured(1) == "y")
      box.translate(0, translated(parts[0], box.height()));
    else
      box.translate(translated(parts[0], box.width()),
                    match.captured(1).isEmpty() && parts.size() > 1 ? translated(parts[1], box.height()) : 0);
  }
  return box;
}

inline QRectF generatedIconRect(const GeneratedContentStyle& value, QRectF box, qreal zoom) {
  if (!value.rule.svgFromMask || !value.rule.computed) return box;
  const auto& style = *value.rule.computed;
  auto size = style.resolvedValue("mask-size");
  if (size.isEmpty()) size = style.resolvedValue("-webkit-mask-size");
  const auto parts = splitTopLevelSpaces(size);
  const CssLengthContext context{style.fontSizePx * style.textScale, style.rootFontSizePx * style.textScale, style.fontSizePx * .5,
                                 style.fontSizePx * .5, style.viewportPx};
  QRectF image = box;
  if (!parts.isEmpty()) {
    const auto used = [&](const QString& text, qreal basis) {
      const auto length = parseCssLengthPercentage(QStringView(text), context);
      return length.status == CssLengthStatus::Valid ? length.px * zoom + length.fraction * basis : basis;
    };
    image.setWidth(used(parts[0], box.width()));
    image.setHeight(used(parts.size() > 1 ? parts[1] : "auto", box.height()));
  }
  auto position = style.resolvedValue("mask-position");
  if (position.isEmpty()) position = style.resolvedValue("-webkit-mask-position");
  if (position.contains("center"))
    image.moveCenter(box.center());
  else {
    if (position.contains("right")) image.moveRight(box.right());
    if (position.contains("bottom")) image.moveBottom(box.bottom());
  }
  return image;
}

}  // namespace muffin
