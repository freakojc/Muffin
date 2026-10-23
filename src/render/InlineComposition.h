#pragma once

#include <QColor>
#include <QString>
#include <QTextLayout>

namespace muffin {

// A shaping snapshot: source/display offsets remain pristine while preedit
// text and its formatting are spliced into the same output value.
struct InlineComposition {
  QString text;
  QVector<QTextLayout::FormatRange> formats;
  int insertAt = -1;
  int length = 0;
  bool active() const { return length > 0; }
  int toLayoutOffset(int displayOffset) const {
    return active() && displayOffset > insertAt ? displayOffset + length : displayOffset;
  }
  int toDisplayOffset(int layoutOffset) const {
    if (!active() || layoutOffset <= insertAt)
      return layoutOffset;
    return layoutOffset <= insertAt + length ? insertAt : layoutOffset - length;
  }
  int cursorOffset(int cursor) const { return insertAt + qBound(0, cursor, length); }
};
InlineComposition composeInlineText(QString text, QVector<QTextLayout::FormatRange> formats, int insertAt,
                                    const QString& preeditText, const QVector<QTextLayout::FormatRange>& preeditFormats,
                                    QColor baseForeground);

} // namespace muffin
