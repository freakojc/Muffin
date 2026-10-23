#include "theme/CssFilter.h"
#include "theme/CssValueParser.h"
#include <QRegularExpression>
#include <QtMath>
#include <cmath>

namespace muffin {
CssFilterOperation identityCssFilter(CssFilterOperation::Kind kind) {
  using K = CssFilterOperation::Kind;
  CssFilterOperation op;
  op.kind = kind;
  op.value = kind == K::Brightness || kind == K::Contrast || kind == K::Opacity || kind == K::Saturate ? 1 : 0;
  if (kind == K::DropShadow)
    op.color = Qt::transparent;
  return op;
}
std::optional<std::vector<CssFilterOperation>> parseCssFilters(const QString& raw, const CssLengthContext& context,
                                                               QColor currentColor) {
  const auto text = raw.trimmed();
  std::vector<CssFilterOperation> result;
  if (text.isEmpty() || text.compare("none", Qt::CaseInsensitive) == 0)
    return result;
  qsizetype position = 0;
  while (position < text.size()) {
    while (position < text.size() && text[position].isSpace())
      ++position;
    if (position == text.size())
      break;
    const auto open = text.indexOf('(', position);
    if (open < 0)
      return {};
    const auto name = text.mid(position, open - position).toLower();
    qsizetype end = open + 1;
    int depth = 1;
    for (; end < text.size() && depth; ++end) {
      if (text[end] == '(')
        ++depth;
      else if (text[end] == ')')
        --depth;
    }
    if (depth)
      return {};
    const auto argument = text.mid(open + 1, end - open - 2).trimmed();
    position = end;
    using K = CssFilterOperation::Kind;
    static const QHash<QString, K> names{{"blur", K::Blur},         {"brightness", K::Brightness},
                                         {"contrast", K::Contrast}, {"grayscale", K::Grayscale},
                                         {"sepia", K::Sepia},       {"hue-rotate", K::HueRotate},
                                         {"opacity", K::Opacity},   {"invert", K::Invert},
                                         {"saturate", K::Saturate}, {"drop-shadow", K::DropShadow}};
    if (!names.contains(name))
      return {};
    auto op = identityCssFilter(names.value(name));
    const auto length = [&](const QString& value) -> std::optional<qreal> {
      const auto parsed = parseCssLengthPercentage(QStringView(value), context);
      if (parsed.status != CssLengthStatus::Valid || parsed.hasPercentage || !std::isfinite(parsed.px))
        return {};
      return parsed.px;
    };
    if (op.kind == K::DropShadow) {
      QVector<qreal> lengths;
      bool colored = false;
      op.color = currentColor;
      for (const auto& part : splitTopLevelSpaces(argument)) {
        if (const auto value = length(part))
          lengths.push_back(*value);
        else if (!colored && isCssColorValue(part)) {
          op.color = part.compare("currentColor", Qt::CaseInsensitive) == 0 ? currentColor : cssColor(part);
          colored = true;
        } else
          return {};
      }
      if (lengths.size() < 2 || lengths.size() > 3 || (lengths.size() == 3 && lengths[2] < 0))
        return {};
      op.offset = {lengths[0], lengths[1]};
      op.value = lengths.size() == 3 ? lengths[2] : 0;
    } else if (op.kind == K::Blur) {
      const auto value = argument.isEmpty() ? std::optional<qreal>(0) : length(argument);
      if (!value || *value < 0)
        return {};
      op.value = *value;
    } else if (op.kind == K::HueRotate) {
      static const QRegularExpression angle("^([+-]?(?:[0-9]*\\.)?[0-9]+)(deg|rad|grad|turn)?$");
      const auto match = angle.match(argument.isEmpty() ? QString("0") : argument.toLower());
      if (!match.hasMatch())
        return {};
      op.value = match.captured(1).toDouble();
      const auto unit = match.captured(2);
      if (unit.isEmpty() && op.value != 0)
        return {};
      if (unit == "rad")
        op.value *= 180 / M_PI;
      else if (unit == "grad")
        op.value *= .9;
      else if (unit == "turn")
        op.value *= 360;
      if (!std::isfinite(op.value))
        return {};
    } else {
      auto number = argument.isEmpty() ? QString("1") : argument;
      const bool percent = number.endsWith('%');
      if (percent)
        number.chop(1);
      bool valid = false;
      op.value = number.toDouble(&valid);
      if (percent)
        op.value /= 100;
      if (!valid || !std::isfinite(op.value) || op.value < 0)
        return {};
      if (op.kind == K::Grayscale || op.kind == K::Sepia || op.kind == K::Invert || op.kind == K::Opacity)
        op.value = qMin<qreal>(1, op.value);
    }
    result.push_back(op);
  }
  return result.empty() ? std::nullopt : std::optional(result);
}
} // namespace muffin
