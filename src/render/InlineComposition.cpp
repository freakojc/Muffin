#include "render/InlineComposition.h"

#include <utility>

namespace muffin {

InlineComposition composeInlineText(QString text, QVector<QTextLayout::FormatRange> formats, int insertAt,
                                    const QString& preeditText, const QVector<QTextLayout::FormatRange>& preeditFormats,
                                    QColor baseForeground) {
  InlineComposition result{std::move(text), std::move(formats)};
  if (insertAt < 0 || preeditText.isEmpty())
    return result;
  insertAt = qBound(0, insertAt, static_cast<int>(result.text.size()));
  const int preeditLen = static_cast<int>(preeditText.size());
  QVector<QTextLayout::FormatRange> shifted;
  shifted.reserve(result.formats.size() + preeditFormats.size() + 1);
  for (const QTextLayout::FormatRange& fr : result.formats) {
    const int s = fr.start;
    const int end = s + fr.length;
    if (fr.length <= 0 || end <= insertAt) {
      shifted.append(fr);
    } else if (s >= insertAt) {
      shifted.append({s + preeditLen, fr.length, fr.format});
    } else { // straddle: split — keep [s, insertAt) and [insertAt+P, end); never cover the preedit
      shifted.append({s, insertAt - s, fr.format});
      shifted.append({insertAt + preeditLen, end - insertAt, fr.format});
    }
  }
  if (preeditFormats.isEmpty()) {
    QTextCharFormat underline;
    underline.setForeground(baseForeground);
    underline.setFontUnderline(true);
    shifted.append({insertAt, preeditLen, underline});
  } else {
    for (const QTextLayout::FormatRange& pf : preeditFormats) {
      QTextCharFormat fmt = pf.format;
      if (!fmt.hasProperty(QTextFormat::ForegroundBrush)) {
        fmt.setForeground(baseForeground);
      }
      shifted.append({insertAt + pf.start, pf.length, fmt});
    }
  }
  result.formats = std::move(shifted);
  result.text.insert(insertAt, preeditText);
  result.insertAt = insertAt;
  result.length = preeditLen;
  return result;
}

} // namespace muffin
