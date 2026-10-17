#pragma once

#include "render/TextLayout.h"

#include <QVector>
#include <memory>
#include <vector>

namespace muffin {

// A source block's immutable shaping result. All coordinates are content-local;
// moving a block or scrolling it only changes the caller's origin, never shaping.
// Preedit is a display splice: source offsets still address the committed text.
class LiteralTextLayout {
public:
  struct Preedit {
    qsizetype offset = 0;
    QString text;
    QList<TextLayout::FormatRange> formats;
    int cursor = -1;
  };
  struct VisualLine {
    qsizetype start = 0;
    qsizetype length = 0;
    QRectF rect;
    TextLine line;
    const TextLayout* layout = nullptr;
    // Qt may collapse a run of trailing breakable spaces onto the line end
    // when the line reaches its wrap width. Keep their logical caret advance
    // explicitly so painting, selection and hit testing remain invertible.
    qsizetype trailingStart = -1;
    qreal trailingSpaceAdvance = 0;
    qreal cursorX(qsizetype displayOffset) const;
  };
  struct NumberedLine {
    int number = 1;
    QRectF rect;
  };

  LiteralTextLayout(QString source, QFont font, qreal width, qreal lineHeight, bool wrap);
  LiteralTextLayout(QString source, QFont font, qreal width, qreal lineHeight, bool wrap, Preedit preedit);
  bool matches(const QString& source, const QFont& font, qreal width, qreal lineHeight, bool wrap) const;
  const QFont& font() const { return font_; }
  qreal height() const { return height_; }
  qreal maximumLineWidth() const { return maximumLineWidth_; }
  bool wraps() const { return wrap_; }
  const Preedit& preedit() const { return preedit_; }
  bool hasPreedit() const { return !preedit_.text.isEmpty(); }
  const QVector<VisualLine>& lines() const { return lines_; }
  const QVector<NumberedLine>& numberedLines() const { return numberedLines_; }

  int lineIndexForOffset(qsizetype sourceOffset) const;
  qsizetype offsetAtPoint(QPointF point) const;
  qsizetype offsetAtLineX(int lineIndex, qreal x) const;
  QRectF cursorRect(qsizetype sourceOffset) const;
  QRectF preeditCursorRect() const;
  QVector<QRectF> selectionRects(qsizetype start, qsizetype end) const;
  void draw(QPainter& painter, QPointF origin, const QList<TextLayout::FormatRange>& sourceFormats) const;

private:
  struct Paragraph {
    qsizetype start = 0;
    qsizetype length = 0;
    std::unique_ptr<TextLayout> layout;
    mutable std::unique_ptr<TextLayout> paintLayout;
    mutable QList<TextLayout::FormatRange> paintFormats;
  };
  qsizetype displayOffset(qsizetype sourceOffset) const;
  qsizetype sourceOffset(qsizetype displayOffset) const;
  int lineIndexForDisplayOffset(qsizetype offset) const;
  QRectF displayCursorRect(qsizetype offset) const;

  QString source_;
  QString display_;
  QFont font_;
  qreal width_ = 1;
  qreal lineHeight_ = 14;
  bool wrap_ = true;
  Preedit preedit_;
  quint64 fontGeneration_ = 0;
  qreal height_ = 0;
  qreal maximumLineWidth_ = 0;
  std::vector<Paragraph> paragraphs_;
  QVector<VisualLine> lines_;
  QVector<NumberedLine> numberedLines_;
};

}  // namespace muffin
