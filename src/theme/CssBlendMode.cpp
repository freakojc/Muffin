#include "theme/CssBlendMode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <QStringList>

namespace muffin {
std::optional<CssBlendMode> parseCssBlendMode(const QString& value) {
  static const QStringList names{"normal",      "multiply",   "screen",     "overlay",    "darken",     "lighten",
                                 "color-dodge", "color-burn", "hard-light", "soft-light", "difference", "exclusion",
                                 "hue",         "saturation", "color",      "luminosity"};
  const auto index = names.indexOf(value.trimmed().toLower());
  return index < 0 ? std::nullopt : std::optional<CssBlendMode>(CssBlendMode(index));
}

std::optional<QPainter::CompositionMode> cssCompositionMode(CssBlendMode mode) {
  static const QPainter::CompositionMode modes[]{
      QPainter::CompositionMode_SourceOver, QPainter::CompositionMode_Multiply,   QPainter::CompositionMode_Screen,
      QPainter::CompositionMode_Overlay,    QPainter::CompositionMode_Darken,     QPainter::CompositionMode_Lighten,
      QPainter::CompositionMode_ColorDodge, QPainter::CompositionMode_ColorBurn,  QPainter::CompositionMode_HardLight,
      QPainter::CompositionMode_SoftLight,  QPainter::CompositionMode_Difference, QPainter::CompositionMode_Exclusion};
  const auto index = int(mode);
  return index < int(std::size(modes)) ? std::optional<QPainter::CompositionMode>(modes[index]) : std::nullopt;
}

namespace {
using Rgb = std::array<double, 3>;
double luminance(Rgb c) {
  return .3 * c[0] + .59 * c[1] + .11 * c[2];
}
double saturation(Rgb c) {
  return *std::max_element(c.begin(), c.end()) - *std::min_element(c.begin(), c.end());
}
Rgb setLuminance(Rgb c, double value) {
  const auto difference = value - luminance(c);
  for (auto& channel : c)
    channel += difference;
  const auto l = luminance(c), n = *std::min_element(c.begin(), c.end()), x = *std::max_element(c.begin(), c.end());
  if (n < 0)
    for (auto& channel : c)
      channel = l + (channel - l) * l / (l - n);
  if (x > 1)
    for (auto& channel : c)
      channel = l + (channel - l) * (1 - l) / (x - l);
  return c;
}
Rgb setSaturation(Rgb c, double value) {
  std::array<int, 3> order{0, 1, 2};
  std::sort(order.begin(), order.end(), [&](int a, int b) { return c[a] < c[b]; });
  auto& lo = c[order[0]];
  auto& mid = c[order[1]];
  auto& hi = c[order[2]];
  if (hi > lo) {
    mid = (mid - lo) * value / (hi - lo);
    hi = value;
  } else {
    mid = hi = 0;
  }
  lo = 0;
  return c;
}
}

void blendNonseparableCssImages(QImage& backdrop, const QImage& source, CssBlendMode mode) {
  Q_ASSERT(backdrop.size() == source.size());
  Q_ASSERT(backdrop.format() == QImage::Format_ARGB32_Premultiplied && source.format() == backdrop.format());
  for (int y = 0; y < backdrop.height(); ++y) {
    auto* destination = reinterpret_cast<QRgb*>(backdrop.scanLine(y));
    const auto* input = reinterpret_cast<const QRgb*>(source.constScanLine(y));
    for (int x = 0; x < backdrop.width(); ++x) {
      const auto s = input[x], b = destination[x];
      if (!qAlpha(s))
        continue;
      const double as = qAlpha(s) / 255., ab = qAlpha(b) / 255.;
      const auto straight = [](QRgb p) -> Rgb {
        const auto a = qAlpha(p);
        return a ? Rgb{double(qRed(p)) / a, double(qGreen(p)) / a, double(qBlue(p)) / a} : Rgb{};
      };
      const auto cs = straight(s), cb = straight(b);
      Rgb blended;
      if (mode == CssBlendMode::Hue)
        blended = setLuminance(setSaturation(cs, saturation(cb)), luminance(cb));
      else if (mode == CssBlendMode::Saturation)
        blended = setLuminance(setSaturation(cb, saturation(cs)), luminance(cb));
      else if (mode == CssBlendMode::Color)
        blended = setLuminance(cs, luminance(cb));
      else
        blended = setLuminance(cb, luminance(cs));
      std::array<int, 3> result;
      for (int i = 0; i < 3; ++i)
        result[i] = qBound(
            0, int(std::lround(255 * (as * (1 - ab) * cs[i] + as * ab * blended[i] + (1 - as) * ab * cb[i]))), 255);
      destination[x] = qRgba(result[0], result[1], result[2], int(std::lround(255 * (as + ab * (1 - as)))));
    }
  }
}
} // namespace muffin
