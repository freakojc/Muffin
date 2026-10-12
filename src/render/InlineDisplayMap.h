#pragma once

#include "projection/InlineProjection.h"

namespace muffin {

// The display stream and its mappings are one value. All rewrites update both
// maps together; layout/paint code never edits an offset table independently.
// IME text is a subsequent shaping transform, outside this projection map.
class InlineDisplayMap {
public:
  struct OffsetMapEntry {
    qsizetype displayStart = 0, displayEnd = 0;
    qsizetype visibleStart = 0, visibleEnd = 0;
  };
  struct DisplayOffsetMapEntry {
    qsizetype projectionStart = 0, projectionEnd = 0;
    qsizetype layoutStart = 0, layoutEnd = 0;
  };
  struct DisplayOffsetRange {
    qsizetype start = 0, end = 0;
    bool valid = false;
  };
  struct Insertions {
    QVector<qsizetype> positions;
    qsizetype before(qsizetype position) const;
    qsizetype after(qsizetype position) const;
  };

  void reset(const InlineProjection& projection);
  void assign(QString text, QVector<OffsetMapEntry> visible, QVector<DisplayOffsetMapEntry> projected);
  const QString& text() const { return displayText_; }
  bool empty() const { return offsetMap_.isEmpty(); }
  qsizetype visibleOffsetForDisplayOffset(qsizetype offset) const;
  qsizetype displayOffsetForVisibleOffset(qsizetype offset) const;
  qsizetype projectionDisplayOffsetForLayoutOffset(qsizetype offset, InlineProjectionBias bias) const;
  qsizetype layoutDisplayOffsetForProjectionOffset(qsizetype offset, InlineProjectionBias bias) const;
  DisplayOffsetRange layoutDisplayRangeForProjectionRange(qsizetype start, qsizetype end) const;
  void insertGenerated(qsizetype position, const QString& text, qsizetype projectedAnchor, qsizetype visibleAnchor);
  Insertions insertSpacing(QVector<qsizetype> positions, QChar placeholder);
  void collapse(DisplayOffsetRange range, QChar placeholder);

private:
  QString displayText_;
  qsizetype visibleLength_ = 0, projectionLength_ = 0;
  QVector<OffsetMapEntry> offsetMap_;
  QVector<DisplayOffsetMapEntry> displayOffsetMap_;
};

}  // namespace muffin
