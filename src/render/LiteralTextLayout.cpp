#include "render/LiteralTextLayout.h"

#include "render/LayoutResources.h"

#include <QPainter>
#include <QTextOption>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace muffin {

qreal LiteralTextLayout::cursorX(const VisualLine& visual, qsizetype offset) const {
  const auto line = cachedParagraph(visual.paragraph).layout->lineAt(visual.row);
  if (!line.isValid())
    return 0;
  if (visual.trailingStart >= 0 && offset >= visual.trailingStart) {
    const qreal base = line.cursorToX(int(visual.trailingStart - visual.start) + line.textStart());
    return base + qreal(offset - visual.trailingStart) * visual.trailingSpaceAdvance;
  }
  return line.cursorToX(int(offset - visual.start) + line.textStart());
}

std::unique_ptr<TextLayout> LiteralTextLayout::shapeParagraph(qsizetype start, qsizetype length) const {
  auto layout = std::make_unique<TextLayout>(length ? display_.mid(start, length) : QStringLiteral(" "), font_);
  layout->setCacheEnabled(true);
  QTextOption option;
  option.setWrapMode(wrap_ ? QTextOption::WrapAtWordBoundaryOrAnywhere : QTextOption::NoWrap);
  option.setFlags(option.flags() | QTextOption::IncludeTrailingSpaces);
  layout->setTextOption(option);
  QList<TextLayout::FormatRange> formats;
  for (const auto& format : preedit_.formats) {
    const qsizetype first = qMax(start, preedit_.offset + qMax(0, format.start));
    const qsizetype last =
        qMin(start + length,
             preedit_.offset + qMin<qsizetype>(preedit_.text.size(), qsizetype(format.start) + format.length));
    if (first < last)
      formats.append({int(first - start), int(last - first), format.format});
  }
  layout->setFormats(formats);
  return layout;
}

LiteralTextLayout::ParagraphLayout& LiteralTextLayout::cachedParagraph(qsizetype index) const {
  if (auto* cached = paragraphCache_.object(index))
    return *cached;
  const auto& paragraph = paragraphs_[index];
  auto cached = std::make_unique<ParagraphLayout>();
  cached->layout = shapeParagraph(paragraph.start, paragraph.length);
  cached->layout->beginLayout();
  for (int row = 0; row < paragraph.lineCount; ++row) {
    auto line = cached->layout->createLine();
    if (!line.isValid())
      break;
    line.setLineWidth(width_);
    const auto& rect = lines_[paragraph.firstLine + row].rect;
    line.setPosition({0, rect.top() + (rect.height() - line.height()) * .5});
  }
  cached->layout->endLayout();
  auto* result = cached.release();
  // Even one giant physical line must remain usable. Cap its cache cost so
  // QCache retains it rather than deleting an entry larger than the budget.
  paragraphCache_.insert(index, result, int(qMin<qsizetype>(paragraph.length + 128, paragraphCache_.maxCost())));
  return *result;
}

LiteralTextLayout::LiteralTextLayout(QString source, QFont font, qreal width, qreal lineHeight, bool wrap)
    : LiteralTextLayout(std::move(source), std::move(font), width, lineHeight, wrap, Preedit{}) {
}

LiteralTextLayout::LiteralTextLayout(QString source, QFont font, qreal width, qreal lineHeight, bool wrap,
                                     Preedit preedit)
    : source_(std::move(source)), display_(source_), font_(std::move(font)), width_(qMax<qreal>(1, width)),
      lineHeight_(qMax<qreal>(14, lineHeight)), wrap_(wrap), preedit_(std::move(preedit)),
      fontGeneration_(LayoutResources::instance().read(LayoutResources::fontKey()).geometry) {
  preedit_.offset = qBound<qsizetype>(0, preedit_.offset, source_.size());
  display_.insert(preedit_.offset, preedit_.text);
  // Keep trailing spaces in Qt's hit-test interval. Without this flag Qt can
  // paint their advances while xToCursor() collapses the whole suffix onto
  // the paragraph end, making a caret round-trip choose the newline offset.
  const qreal defaultSpaceAdvance = TextFontMetrics(font_).horizontalAdvance(QLatin1Char(' '));
  qsizetype start = 0;
  qsizetype countedSource = 0;
  int sourceLineNumber = 1;
  do {
    const qsizetype newline = display_.indexOf(QLatin1Char('\n'), start);
    const qsizetype length = (newline < 0 ? display_.size() : newline) - start;
    auto layout = shapeParagraph(start, length);
    layout->beginLayout();
    const qreal paragraphTop = height_;
    const qsizetype firstVisualLine = lines_.size();
    for (;;) {
      auto line = layout->createLine();
      if (!line.isValid())
        break;
      line.setLineWidth(width_);
      const qreal rowHeight = qMax(lineHeight_, line.height());
      line.setPosition({0, height_ + (rowHeight - line.height()) * .5});
      lines_.append({start + line.textStart(), qMin<qsizetype>(line.textLength(), length - line.textStart()),
                     QRectF(0, height_, width_, rowHeight), qsizetype(paragraphs_.size()),
                     int(lines_.size() - firstVisualLine)});
      height_ += rowHeight;
    }
    // QTextLine::textLength() excludes trailing breakable spaces unless the
    // caller asks for IncludeTrailingSpaces.  The painter still draws their
    // advances and cursorToX() accepts them, so retaining only textLength made
    // the end-of-line caret hit a different offset than a click on it.  Infer
    // the complete visual span from the next line's start (or the paragraph
    // end for the final line) after all lines have been materialized.
    const qsizetype paragraphEnd = start + length;
    for (qsizetype i = firstVisualLine; i < lines_.size(); ++i) {
      const qsizetype end =
          i + 1 < lines_.size() && lines_[i + 1].start <= paragraphEnd ? lines_[i + 1].start : paragraphEnd;
      auto& visual = lines_[i];
      const auto line = layout->lineAt(visual.row);
      visual.length = qMax<qsizetype>(0, end - visual.start);
      qsizetype trailingStart = end;
      while (trailingStart > visual.start && display_.at(trailingStart - 1) == QLatin1Char(' '))
        --trailingStart;
      if (trailingStart < end) {
        const qreal base = line.cursorToX(int(trailingStart - visual.start) + line.textStart());
        const qreal measured = line.cursorToX(int(end - visual.start) + line.textStart()) - base;
        // At a hard wrap Qt can report the whole trailing run at one x
        // coordinate. Use the same font/device advance as the document
        // shaper in that case, retaining one caret slot per source space.
        const qreal advance = measured > defaultSpaceAdvance * .5 && std::isfinite(measured)
                                  ? measured / qreal(end - trailingStart)
                                  : defaultSpaceAdvance;
        visual.trailingStart = trailingStart;
        visual.trailingSpaceAdvance = qMax<qreal>(defaultSpaceAdvance / 64, advance);
      }
      maximumLineWidth_ = qMax(maximumLineWidth_, line.naturalTextWidth());
      const auto endX = visual.trailingStart >= 0
                            ? line.cursorToX(int(visual.trailingStart - visual.start) + line.textStart()) +
                                  (end - visual.trailingStart) * visual.trailingSpaceAdvance
                            : line.cursorToX(int(end - visual.start) + line.textStart());
      maximumLineWidth_ = qMax(maximumLineWidth_, std::abs(endX));
    }
    layout->endLayout();
    const qsizetype sourceStart = sourceOffset(start);
    while (countedSource < sourceStart) {
      if (source_.at(countedSource++) == QLatin1Char('\n'))
        ++sourceLineNumber;
    }
    numberedLines_.append({sourceLineNumber, QRectF(0, paragraphTop, width_, height_ - paragraphTop)});
    paragraphs_.push_back({start, length, firstVisualLine, int(lines_.size() - firstVisualLine)});
    // Small blocks keep their measured layout; large ones are shaped only for
    // paint/hit testing after this geometry pass, without retaining all Qt engines.
    if (display_.size() <= 64 * 1024) {
      auto* cached = new ParagraphLayout{std::move(layout)};
      paragraphCache_.insert(qsizetype(paragraphs_.size()) - 1, cached, int(length + 128));
    }
    if (newline < 0)
      break;
    start = newline + 1;
  } while (start <= display_.size());
}

bool LiteralTextLayout::matches(const QString& source, const QFont& font, qreal width, qreal lineHeight,
                                bool wrap) const {
  return source == source_ && font == font_ && qAbs(qMax<qreal>(1, width) - width_) < 1e-6 &&
         qMax<qreal>(14, lineHeight) == lineHeight_ && wrap == wrap_ &&
         fontGeneration_ == LayoutResources::instance().read(LayoutResources::fontKey()).geometry;
}

qsizetype LiteralTextLayout::displayOffset(qsizetype offset) const {
  offset = qBound<qsizetype>(0, offset, source_.size());
  return offset > preedit_.offset ? offset + preedit_.text.size() : offset;
}

qsizetype LiteralTextLayout::sourceOffset(qsizetype offset) const {
  if (offset > preedit_.offset)
    offset = qMax(preedit_.offset, offset - preedit_.text.size());
  return qBound<qsizetype>(0, offset, source_.size());
}

int LiteralTextLayout::lineIndexForDisplayOffset(qsizetype offset) const {
  offset = qBound<qsizetype>(0, offset, display_.size());
  // At a shared soft-wrap boundary the offset canonically belongs to the
  // preceding row, matching cursorRect and vertical navigation.
  const auto it = std::lower_bound(lines_.cbegin(), lines_.cend(), offset, [](const VisualLine& line, qsizetype pos) {
    return line.start + line.length < pos;
  });
  return it == lines_.cend() ? int(lines_.size()) - 1 : int(it - lines_.cbegin());
}

int LiteralTextLayout::lineIndexForOffset(qsizetype offset) const {
  return lineIndexForDisplayOffset(displayOffset(offset));
}

qsizetype LiteralTextLayout::offsetAtLineX(int index, qreal x) const {
  if (index < 0 || index >= lines_.size())
    return 0;
  const auto& line = lines_[index];
  const auto& layout = *cachedParagraph(line.paragraph).layout;
  const auto shapedLine = layout.lineAt(line.row);
  const int first = shapedLine.textStart(), last = first + int(line.length);
  const int position = qBound(first, shapedLine.xToCursor(x), last);
  // Qt deliberately excludes the end of a soft-wrapped line from xToCursor.
  // Compare its answer with neighbouring valid carets and both line edges so
  // hit testing is the inverse of the retained cursor geometry at those edges.
  int best = position;
  qreal distance = std::abs(cursorX(line, line.start + best - first) - x);
  const auto consider = [&](int candidate, bool allowTrailingSpace = false) {
    if (candidate < first || candidate > last || (!allowTrailingSpace && !layout.isValidCursorPosition(candidate)))
      return;
    const qreal candidateDistance = std::abs(cursorX(line, line.start + candidate - first) - x);
    if (candidateDistance < distance || (candidateDistance == distance && candidate > best)) {
      best = candidate;
      distance = candidateDistance;
    }
  };
  for (int candidate : {first, last, layout.previousCursorPosition(position), layout.nextCursorPosition(position)})
    consider(candidate);
  if (line.trailingStart >= line.start) {
    const int trailing = first + int(line.trailingStart - line.start);
    for (int candidate = trailing; candidate <= last; ++candidate)
      consider(candidate, true);
  }
  return sourceOffset(line.start + best - first);
}

qsizetype LiteralTextLayout::offsetAtPoint(QPointF point) const {
  const auto it = std::upper_bound(lines_.cbegin(), lines_.cend(), point.y(),
                                   [](qreal y, const VisualLine& line) { return y < line.rect.bottom(); });
  const int index = qMin(int(it - lines_.cbegin()), int(lines_.size()) - 1);
  return offsetAtLineX(index, point.x());
}

QRectF LiteralTextLayout::displayCursorRect(qsizetype offset) const {
  const int index = lineIndexForDisplayOffset(offset);
  if (index < 0)
    return {0, 0, 1, lineHeight_};
  const auto& line = lines_[index];
  return {cursorX(line, qBound(line.start, offset, line.start + line.length)), line.rect.top(), 1, line.rect.height()};
}

QRectF LiteralTextLayout::cursorRect(qsizetype offset) const {
  return displayCursorRect(displayOffset(offset));
}

QRectF LiteralTextLayout::preeditCursorRect() const {
  if (!hasPreedit() || preedit_.cursor < 0)
    return {};
  return displayCursorRect(preedit_.offset + qMin<qsizetype>(preedit_.cursor, preedit_.text.size()));
}

QVector<QRectF> LiteralTextLayout::selectionRects(qsizetype start, qsizetype end) const {
  if (start > end)
    std::swap(start, end);
  start = displayOffset(start);
  end = displayOffset(end);
  QVector<QRectF> result;
  if (start == end)
    return result;
  for (const auto& line : lines_) {
    const qsizetype lineEnd = line.start + line.length;
    const qsizetype first = qMax(start, line.start), last = qMin(end, lineEnd);
    if (first < last) {
      const qreal x1 = cursorX(line, first), x2 = cursorX(line, last);
      result.append({qMin(x1, x2), line.rect.top(), qMax<qreal>(1, std::abs(x2 - x1)), line.rect.height()});
    } else if (end > lineEnd && start <= lineEnd && lineEnd < display_.size() &&
               display_[lineEnd] == QLatin1Char('\n')) {
      const qreal x = cursorX(line, lineEnd);
      result.append({x, line.rect.top(), qMax<qreal>(1, qMin<qreal>(24, width_ - x)), line.rect.height()});
    }
  }
  return result;
}

QPair<qsizetype, qsizetype> LiteralTextLayout::paragraphRangeForY(qreal top, qreal bottom) const {
  // Keep a row of overscan for antialiasing and glyph overhang.
  const auto first = std::lower_bound(numberedLines_.cbegin(), numberedLines_.cend(), top - lineHeight_,
                                      [](const NumberedLine& line, qreal y) { return line.rect.bottom() < y; });
  const auto last = std::upper_bound(first, numberedLines_.cend(), bottom + lineHeight_,
                                     [](qreal y, const NumberedLine& line) { return y < line.rect.top(); });
  return {first - numberedLines_.cbegin(), last - numberedLines_.cbegin()};
}

QPair<qsizetype, qsizetype> LiteralTextLayout::sourceRangeForY(qreal top, qreal bottom) const {
  const auto [first, last] = paragraphRangeForY(top, bottom);
  if (first == last)
    return {0, 0};
  return {sourceOffset(paragraphs_[first].start),
          sourceOffset(paragraphs_[last - 1].start + paragraphs_[last - 1].length)};
}

void LiteralTextLayout::draw(QPainter& painter, QPointF origin,
                             const QList<TextLayout::FormatRange>& sourceFormats) const {
  qsizetype firstParagraph = 0, lastParagraph = qsizetype(paragraphs_.size());
  if (painter.hasClipping()) {
    const auto clip = painter.clipBoundingRect().translated(-origin);
    std::tie(firstParagraph, lastParagraph) = paragraphRangeForY(clip.top(), clip.bottom());
  }
  if (firstParagraph == lastParagraph)
    return;
  const qsizetype visibleStart = paragraphs_[firstParagraph].start;
  const qsizetype visibleEnd = paragraphs_[lastParagraph - 1].start + paragraphs_[lastParagraph - 1].length;
  std::vector<QList<TextLayout::FormatRange>> formats(lastParagraph - firstParagraph);
  // Distribute each range only to the physical lines it overlaps.
  const auto append = [&](qsizetype start, qsizetype end, const QTextCharFormat& format) {
    start = qMax(start, visibleStart);
    end = qMin(end, visibleEnd);
    if (start >= end)
      return;
    auto it = std::upper_bound(paragraphs_.begin(), paragraphs_.end(), start,
                               [](qsizetype pos, const Paragraph& paragraph) { return pos < paragraph.start; });
    if (it != paragraphs_.begin())
      --it;
    for (; it != paragraphs_.end() && it->start < end; ++it) {
      const qsizetype first = qMax(start, it->start), last = qMin(end, it->start + it->length);
      if (first < last)
        formats[it - paragraphs_.begin() - firstParagraph].append({int(first - it->start), int(last - first), format});
    }
  };
  QTextCharFormat defaultInk;
  defaultInk.setForeground(painter.pen().brush());
  for (qsizetype i = 0; i < sourceFormats.size(); ++i) {
    const auto& range = sourceFormats[i];
    // Plain source needs no formatted variant when its ink is already the pen.
    // Only omit the initial base; later ranges can restore ink overridden above.
    if (i == 0 && range.start == 0 && range.length == source_.size() && range.format == defaultInk)
      continue;
    const qsizetype start = qBound<qsizetype>(0, qsizetype(range.start), source_.size());
    const qsizetype end = qBound<qsizetype>(start, qsizetype(range.start) + range.length, source_.size());
    if (!hasPreedit())
      append(start, end, range.format);
    else {
      append(start, qMin(end, preedit_.offset), range.format);
      const qsizetype after = qMax(start, preedit_.offset);
      append(after + preedit_.text.size(), end + preedit_.text.size(), range.format);
    }
  }
  for (const auto& range : preedit_.formats)
    append(preedit_.offset + qMax(0, range.start),
           preedit_.offset + qMin<qsizetype>(preedit_.text.size(), qsizetype(range.start) + range.length),
           range.format);
  for (qsizetype i = firstParagraph; i < lastParagraph; ++i) {
    auto& paragraph = cachedParagraph(i);
    const auto& paragraphFormats = formats[i - firstParagraph];
    if (paragraphFormats == paragraph.layout->formats()) {
      paragraph.paintLayout.reset();
      paragraph.paintFormats.clear();
      paragraph.layout->draw(&painter, origin);
      continue;
    }
    // These are text formats, not selections. QTextLayout::draw(selections)
    // suppresses overlapping foreground regions, including adjacent character
    // ranges whose selection rectangles overlap after rounding to pixels.
    // Cache the formatted variant so repainting never repeats this shaping.
    if (!paragraph.paintLayout || paragraph.paintFormats != paragraphFormats) {
      auto layout = std::make_unique<TextLayout>(paragraph.layout->text(), font_, paragraph.layout->backend());
      layout->setCacheEnabled(true);
      layout->setTextOption(paragraph.layout->textOption());
      layout->setFormats(paragraphFormats);
      layout->beginLayout();
      for (int row = 0; row < paragraph.layout->lineCount(); ++row) {
        const auto original = paragraph.layout->lineAt(row);
        auto line = layout->createLine();
        if (!line.isValid())
          break;
        line.setLineWidth(original.width());
        line.setPosition(original.position());
      }
      layout->endLayout();
      paragraph.paintFormats = paragraphFormats;
      paragraph.paintLayout = std::move(layout);
    }
    paragraph.paintLayout->draw(&painter, origin);
  }
}

} // namespace muffin
