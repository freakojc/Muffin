#include "render/InlineDisplayMap.h"

#include <algorithm>
#include <utility>

namespace muffin {

void InlineDisplayMap::reset(const InlineProjection& projection) {
  visibleLength_ = projection.visibleText().size();
  projectionLength_ = projection.displayText().size();
  displayText_ = projection.displayText();
  offsetMap_.clear();
  displayOffsetMap_.clear();
  offsetMap_.reserve(projection.spans().size());
  displayOffsetMap_.reserve(projection.spans().size());
  for (const InlineProjectionSpan& span : projection.spans()) {
    offsetMap_.push_back(OffsetMapEntry{span.displayStart, span.displayEnd, span.visibleStart, span.visibleEnd});
    displayOffsetMap_.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, span.displayStart, span.displayEnd});
  }
}

qsizetype InlineDisplayMap::visibleOffsetForDisplayOffset(qsizetype displayOffset) const {
  if (offsetMap_.isEmpty()) {
    return qBound<qsizetype>(0, displayOffset, visibleLength_);
  }
  displayOffset = qBound<qsizetype>(0, displayOffset, displayText_.size());
  for (const OffsetMapEntry& entry : offsetMap_) {
    if (displayOffset <= entry.displayEnd) {
      if (entry.visibleEnd <= entry.visibleStart || entry.displayEnd <= entry.displayStart) {
        return entry.visibleStart;
      }
      const qsizetype delta = qBound<qsizetype>(0, displayOffset - entry.displayStart, entry.visibleEnd - entry.visibleStart);
      return qBound<qsizetype>(entry.visibleStart, entry.visibleStart + delta, entry.visibleEnd);
    }
  }
  return visibleLength_;
}

qsizetype InlineDisplayMap::displayOffsetForVisibleOffset(qsizetype visibleOffset) const {
  if (offsetMap_.isEmpty()) {
    return qBound<qsizetype>(0, visibleOffset, visibleLength_);
  }
  visibleOffset = qBound<qsizetype>(0, visibleOffset, visibleLength_);
  for (const OffsetMapEntry& entry : offsetMap_) {
    if (visibleOffset == entry.visibleEnd && visibleOffset < visibleLength_) continue;
    if (visibleOffset <= entry.visibleEnd) {
      if (entry.visibleEnd <= entry.visibleStart || entry.displayEnd <= entry.displayStart) {
        continue;
      }
      const qsizetype delta = qBound<qsizetype>(0, visibleOffset - entry.visibleStart, entry.displayEnd - entry.displayStart);
      return qBound<qsizetype>(entry.displayStart, entry.displayStart + delta, entry.displayEnd);
    }
  }
  return displayText_.size();
}

qsizetype InlineDisplayMap::projectionDisplayOffsetForLayoutOffset(qsizetype layoutOffset, InlineProjectionBias bias) const {
  if (displayOffsetMap_.isEmpty()) {
    return qBound<qsizetype>(0, layoutOffset, projectionLength_);
  }

  layoutOffset = qBound<qsizetype>(0, layoutOffset, displayText_.size());
  for (const DisplayOffsetMapEntry& entry : displayOffsetMap_) {
    if (layoutOffset < entry.layoutStart || layoutOffset > entry.layoutEnd) {
      continue;
    }
    if (entry.layoutEnd <= entry.layoutStart || entry.projectionEnd <= entry.projectionStart) {
      return bias == InlineProjectionBias::Forward ? entry.projectionEnd : entry.projectionStart;
    }
    if (layoutOffset <= entry.layoutStart) {
      return entry.projectionStart;
    }
    if (layoutOffset >= entry.layoutEnd) {
      return entry.projectionEnd;
    }
    const qsizetype projectionLength = entry.projectionEnd - entry.projectionStart;
    const qsizetype layoutLength = entry.layoutEnd - entry.layoutStart;
    const qsizetype delta = (layoutOffset - entry.layoutStart) * projectionLength / layoutLength;
    return qBound<qsizetype>(entry.projectionStart, entry.projectionStart + delta, entry.projectionEnd);
  }

  return projectionLength_;
}

qsizetype InlineDisplayMap::layoutDisplayOffsetForProjectionOffset(qsizetype projectionOffset, InlineProjectionBias bias) const {
  if (displayOffsetMap_.isEmpty()) {
    return qBound<qsizetype>(0, projectionOffset, displayText_.size());
  }

  projectionOffset = qBound<qsizetype>(0, projectionOffset, projectionLength_);
  qsizetype generatedFallback = -1;
  for (const DisplayOffsetMapEntry& entry : displayOffsetMap_) {
    if (projectionOffset < entry.projectionStart || projectionOffset > entry.projectionEnd) {
      continue;
    }
    // Shared boundaries belong to the following editable run, beyond its
    // zero-source spacers/prefix. Apply this for source selections as well as
    // carets; the bias still controls positions inside collapsed atoms.
    if (projectionOffset == entry.projectionEnd &&
        projectionOffset < projectionLength_) continue;
    if (entry.projectionEnd <= entry.projectionStart) {
      // Generated content has no source extent. Prefer the adjacent editable
      // range for both caret bias and selections; only an empty source needs
      // this fallback (the boundary between ::before and ::after).
      if (generatedFallback < 0) generatedFallback = entry.layoutEnd;
      continue;
    }
    if (entry.layoutEnd <= entry.layoutStart) {
      return bias == InlineProjectionBias::Forward ? entry.layoutEnd : entry.layoutStart;
    }
    if (projectionOffset <= entry.projectionStart) {
      return entry.layoutStart;
    }
    if (projectionOffset >= entry.projectionEnd) {
      return entry.layoutEnd;
    }
    const qsizetype projectionLength = entry.projectionEnd - entry.projectionStart;
    const qsizetype layoutLength = entry.layoutEnd - entry.layoutStart;
    const qsizetype delta = (projectionOffset - entry.projectionStart) * layoutLength / projectionLength;
    const qsizetype snapped = bias == InlineProjectionBias::Forward && delta == 0 ? 1 : delta;
    return qBound<qsizetype>(entry.layoutStart, entry.layoutStart + snapped, entry.layoutEnd);
  }

  return generatedFallback >= 0 ? generatedFallback : displayText_.size();
}

InlineDisplayMap::DisplayOffsetRange InlineDisplayMap::layoutDisplayRangeForProjectionRange(qsizetype projectionStart, qsizetype projectionEnd) const {
  DisplayOffsetRange range;
  if (projectionEnd <= projectionStart) {
    return range;
  }
  // Select content entries, excluding generated spacing at either boundary.
  // Shared boundary offsets can denote both the preceding run and the spacer;
  // mapping a whole span through one caret bias would include padding as text.
  bool found = false;
  for (const auto& entry : displayOffsetMap_) {
    if (entry.projectionEnd <= entry.projectionStart || entry.projectionEnd <= projectionStart || entry.projectionStart >= projectionEnd)
      continue;
    const qsizetype a = qMax(projectionStart, entry.projectionStart), b = qMin(projectionEnd, entry.projectionEnd);
    const qsizetype length = entry.projectionEnd - entry.projectionStart;
    const qsizetype start = entry.layoutStart + (a - entry.projectionStart) * (entry.layoutEnd - entry.layoutStart) / length;
    const qsizetype end = entry.layoutStart + (b - entry.projectionStart) * (entry.layoutEnd - entry.layoutStart) / length;
    if (!found) {
      range.start = start;
      range.end = end;
      found = true;
    } else {
      range.start = qMin(range.start, start);
      range.end = qMax(range.end, end);
    }
  }
  if (!found) return range;
  range.start = qBound<qsizetype>(0, range.start, displayText_.size());
  range.end = qBound<qsizetype>(0, range.end, displayText_.size());
  range.valid = range.end > range.start;
  return range;
}

void InlineDisplayMap::assign(QString text, QVector<OffsetMapEntry> visible, QVector<DisplayOffsetMapEntry> projected) {
  displayText_ = std::move(text);
  offsetMap_ = std::move(visible);
  displayOffsetMap_ = std::move(projected);
}

qsizetype InlineDisplayMap::Insertions::before(qsizetype position) const {
  return position + (std::lower_bound(positions.begin(), positions.end(), position) - positions.begin());
}
qsizetype InlineDisplayMap::Insertions::after(qsizetype position) const {
  return position + (std::upper_bound(positions.begin(), positions.end(), position) - positions.begin());
}

void InlineDisplayMap::insertGenerated(qsizetype position, const QString& inserted, qsizetype projectionAnchor, qsizetype visibleAnchor) {
  const qsizetype length = inserted.size();
  const auto shift = [&](qsizetype& start, qsizetype& end) {
    if (start >= position) {
      start += length;
      end += length;
    } else if (end > position) {
      end += length;
    }
  };
  QVector<OffsetMapEntry> offsets;
  for (auto entry : offsetMap_) {
    if (entry.displayStart < position && position < entry.displayEnd) {
      const auto anchor = entry.visibleStart + (entry.visibleEnd - entry.visibleStart) *
          (position - entry.displayStart) / (entry.displayEnd - entry.displayStart);
      offsets.push_back({entry.displayStart, position, entry.visibleStart, anchor});
      offsets.push_back({position + length, entry.displayEnd + length, anchor, entry.visibleEnd});
    } else { shift(entry.displayStart, entry.displayEnd); offsets.push_back(entry); }
  }
  offsetMap_ = std::move(offsets);
  QVector<DisplayOffsetMapEntry> displayOffsets;
  for (auto entry : displayOffsetMap_) {
    if (entry.layoutStart < position && position < entry.layoutEnd) {
      displayOffsets.push_back({entry.projectionStart, projectionAnchor, entry.layoutStart, position});
      displayOffsets.push_back({projectionAnchor, entry.projectionEnd, position + length, entry.layoutEnd + length});
    } else { shift(entry.layoutStart, entry.layoutEnd); displayOffsets.push_back(entry); }
  }
  displayOffsetMap_ = std::move(displayOffsets);
  displayText_.insert(position, inserted);
  offsetMap_.push_back({position, position + length, visibleAnchor, visibleAnchor});
  displayOffsetMap_.push_back({projectionAnchor, projectionAnchor, position, position + length});
  std::stable_sort(offsetMap_.begin(), offsetMap_.end(), [](const auto& a, const auto& b) {
    return a.displayStart < b.displayStart;
  });
  std::stable_sort(displayOffsetMap_.begin(), displayOffsetMap_.end(), [](const auto& a, const auto& b) {
    return a.layoutStart < b.layoutStart;
  });
}

InlineDisplayMap::Insertions InlineDisplayMap::insertSpacing(QVector<qsizetype> positions, QChar placeholder) {
  std::sort(positions.begin(), positions.end());
  positions.erase(std::unique(positions.begin(), positions.end()), positions.end());
  Insertions result{std::move(positions)};
  const auto before = [&](qsizetype p) { return result.before(p); };
  const auto after = [&](qsizetype p) { return result.after(p); };
  QVector<OffsetMapEntry> offsets;
  QVector<DisplayOffsetMapEntry> displayOffsets;
  for (const auto& e : offsetMap_) {
    qsizetype start = e.displayStart;
    const auto visible = [&](qsizetype p) {
      return e.visibleStart +
             (e.displayEnd > e.displayStart ? (e.visibleEnd - e.visibleStart) * (p - e.displayStart) / (e.displayEnd - e.displayStart) : 0);
    };
    for (auto position : result.positions)
      if (position > start && position < e.displayEnd) {
        offsets.push_back({after(start), before(position), visible(start), visible(position)});
        start = position;
      }
    offsets.push_back({after(start), e.displayEnd > start ? before(e.displayEnd) : after(e.displayEnd), visible(start), e.visibleEnd});
  }
  for (const auto& e : displayOffsetMap_) {
    qsizetype start = e.layoutStart;
    const auto projected = [&](qsizetype p) {
      return e.projectionStart + (e.layoutEnd > e.layoutStart
                                      ? (e.projectionEnd - e.projectionStart) * (p - e.layoutStart) / (e.layoutEnd - e.layoutStart)
                                      : 0);
    };
    for (auto position : result.positions)
      if (position > start && position < e.layoutEnd) {
        displayOffsets.push_back({projected(start), projected(position), after(start), before(position)});
        start = position;
      }
    displayOffsets.push_back(
        {projected(start), e.projectionEnd, after(start), e.layoutEnd > start ? before(e.layoutEnd) : after(e.layoutEnd)});
  }
  for (auto position : result.positions) {
    const qsizetype pos = before(position);
    const qsizetype visible = visibleOffsetForDisplayOffset(position);
    const qsizetype projected = projectionDisplayOffsetForLayoutOffset(position, InlineProjectionBias::Backward);
    offsets.push_back({pos, pos + 1, visible, visible});
    displayOffsets.push_back({projected, projected, pos, pos + 1});

  }
  for (auto it = result.positions.rbegin(); it != result.positions.rend(); ++it) displayText_.insert(*it, placeholder);
  std::stable_sort(offsets.begin(), offsets.end(), [](const auto& a, const auto& b) { return a.displayStart < b.displayStart; });
  std::stable_sort(displayOffsets.begin(), displayOffsets.end(),
                   [](const auto& a, const auto& b) { return a.layoutStart < b.layoutStart; });
  offsetMap_ = std::move(offsets);
  displayOffsetMap_ = std::move(displayOffsets);
  return result;
}

void InlineDisplayMap::collapse(DisplayOffsetRange range, QChar placeholder) {
  const qsizetype delta = 1 - (range.end - range.start);
  displayText_.replace(range.start, range.end - range.start, placeholder);
  for (auto& entry : offsetMap_) {
    if (entry.displayStart == range.start && entry.displayEnd == range.end)
      entry.displayEnd = range.start + 1;
    else if (entry.displayStart >= range.end) {
      entry.displayStart += delta;
      entry.displayEnd += delta;
    }
  }
  for (auto& entry : displayOffsetMap_) {
    if (entry.layoutStart == range.start && entry.layoutEnd == range.end)
      entry.layoutEnd = range.start + 1;
    else if (entry.layoutStart >= range.end) {
      entry.layoutStart += delta;
      entry.layoutEnd += delta;
    }
  }
}

}  // namespace muffin
