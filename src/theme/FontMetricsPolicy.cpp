#include "theme/FontMetricsPolicy.h"

#include <QRawFont>
#include <QtEndian>
#include <cmath>

qreal muffin::font_rendering::syntheticBoldAdvanceCorrection(const QFont& font) {
#if defined(Q_OS_WIN)
  if (font.weight() < QFont::DemiBold) return 0;
  const auto bold = QRawFont::fromFont(font);
  const auto os2 = bold.fontTable("OS/2");
  if (!bold.isValid() || os2.size() < 6 || !bold.fontTable("fvar").isEmpty()) return 0;
  const auto faceWeight = qFromBigEndian<quint16>(os2.constData() + 4);
  if (faceWeight < 1 || faceWeight >= QFont::DemiBold || faceWeight >= font.weight()) return 0;
  auto originalFont = font;
  originalFont.setWeight(static_cast<QFont::Weight>(faceWeight));
  const auto original = QRawFont::fromFont(originalFont);
  const auto glyphs = bold.glyphIndexesForString(QStringLiteral("HMn "));
  if (!original.isValid() || bold.familyName() != original.familyName() ||
      bold.fontTable("head") != original.fontTable("head") || glyphs.size() != 4 || glyphs.contains(0))
    return 0;
  const auto simulated = bold.advancesForGlyphIndexes(glyphs);
  const auto unsimulated = original.advancesForGlyphIndexes(glyphs);
  qreal minimum = 1e6, maximum = 0, sum = 0;
  for (int i = 0; i < 3; ++i) {
    const qreal delta = simulated[i].x() - unsimulated[i].x();
    minimum = qMin(minimum, delta);
    maximum = qMax(maximum, delta);
    sum += delta;
  }
  if (minimum > 1.0 / 64 && maximum - minimum <= 2.0 / 64 && maximum < bold.pixelSize() / 10 &&
      qAbs(simulated[3].x() - unsimulated[3].x()) <= 1.0 / 64)
    return std::round(sum / 3 * 64) / 64;
#else
  Q_UNUSED(font);
#endif
  return 0;
}
