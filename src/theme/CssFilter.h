#pragma once

#include "theme/CssCalc.h"
#include <QColor>
#include <QPointF>
#include <optional>
#include <vector>

namespace muffin {
struct CssFilterOperation {
  enum class Kind { Blur, Brightness, Contrast, Grayscale, Sepia, HueRotate, Opacity, Invert, Saturate, DropShadow };
  Kind kind = Kind::Blur;
  qreal value = 0;
  QPointF offset;
  QColor color;
  bool operator==(const CssFilterOperation&) const = default;
};
// A malformed function invalidates the entire declaration. The same grammar
// is used by @supports, cascade validation, and computed paint projection.
std::optional<std::vector<CssFilterOperation>> parseCssFilters(const QString& value, const CssLengthContext& context = {},
                                                             QColor currentColor = Qt::black);
CssFilterOperation identityCssFilter(CssFilterOperation::Kind kind);
}  // namespace muffin
