#pragma once

#include <QImage>
#include <QPainter>
#include <QString>
#include <optional>

namespace muffin {
enum class CssBlendMode { Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, ColorBurn,
                          HardLight, SoftLight, Difference, Exclusion, Hue, Saturation, Color, Luminosity };
std::optional<CssBlendMode> parseCssBlendMode(const QString& value);
std::optional<QPainter::CompositionMode> cssCompositionMode(CssBlendMode mode);
// W3C nonseparable blend functions, followed by source-over alpha compositing.
// Images must have matching sizes and premultiplied ARGB32 storage.
void blendNonseparableCssImages(QImage& backdrop, const QImage& source, CssBlendMode mode);
}
