#include "document/InlineNode.h"
#include <QAnyStringView>

#include <limits>
#include <utility>

namespace muffin {
namespace {
const std::array<QString, 13>& commonMarkers() {
  static const std::array<QString, 13> markers{QString(),
                                               QStringLiteral("*"),
                                               QStringLiteral("_"),
                                               QStringLiteral("**"),
                                               QStringLiteral("__"),
                                               QStringLiteral("~~"),
                                               QStringLiteral("=="),
                                               QStringLiteral("~"),
                                               QStringLiteral("^"),
                                               QStringLiteral("`"),
                                               QStringLiteral("``"),
                                               QStringLiteral("$"),
                                               QStringLiteral("$$")};
  return markers;
}
} // namespace

bool InlineRange::isValid() const {
  return start >= 0 && end >= start;
}

qsizetype InlineRange::length() const {
  return isValid() ? end - start : 0;
}

InlineNode::InlineNode(InlineType type) : type_(type) {
}

InlineNode::InlineNode(const InlineNode& other)
    : type_(other.type_), markerKind_(other.markerKind_), text_(other.text_), children_(other.children_),
      sourceRanges_(other.sourceRanges_),
      extendedMetadata_(other.extendedMetadata_ ? std::make_unique<ExtendedMetadata>(*other.extendedMetadata_)
                                                : nullptr) {
}

InlineNode& InlineNode::operator=(const InlineNode& other) {
  if (this != &other) {
    auto extendedMetadata =
        other.extendedMetadata_ ? std::make_unique<ExtendedMetadata>(*other.extendedMetadata_) : nullptr;
    type_ = other.type_;
    markerKind_ = other.markerKind_;
    text_ = other.text_;
    children_ = other.children_;
    sourceRanges_ = other.sourceRanges_;
    extendedMetadata_ = std::move(extendedMetadata);
  }
  return *this;
}

InlineNode::ExtendedMetadata::ExtendedMetadata(const ExtendedMetadata& other)
    : marker(other.marker), href(other.href), title(other.title), alt(other.alt), autolink(other.autolink),
      wideRanges(other.wideRanges ? std::make_unique<InlineSourceRanges>(*other.wideRanges) : nullptr) {
}

bool InlineNode::ExtendedMetadata::isEmpty() const {
  return marker.isEmpty() && href.isEmpty() && title.isEmpty() && alt.isEmpty() && !autolink && !wideRanges;
}

InlineNode::ExtendedMetadata& InlineNode::ensureExtendedMetadata() {
  if (!extendedMetadata_) {
    extendedMetadata_ = std::make_unique<ExtendedMetadata>();
  }
  return *extendedMetadata_;
}

void InlineNode::pruneExtendedMetadata() {
  if (extendedMetadata_ && extendedMetadata_->isEmpty()) {
    extendedMetadata_.reset();
  }
}

InlineType InlineNode::type() const {
  return type_;
}

QString InlineNode::text() const {
  if (const auto* owned = std::get_if<QString>(&text_)) {
    return *owned;
  }
  return textView().toString();
}

QStringView InlineNode::textView() const {
  if (const auto* owned = std::get_if<QString>(&text_)) {
    return QStringView(*owned);
  }
  const SharedTextSlice& shared = std::get<SharedTextSlice>(text_);
  return shared.source ? QStringView(*shared.source).mid(shared.start, shared.length) : QStringView();
}

void InlineNode::setText(QString text) {
  text_ = std::move(text);
}

bool InlineNode::bindSharedText(const std::shared_ptr<const QString>& source) {
  if (!source || source->size() > std::numeric_limits<int>::max()) {
    return false;
  }
  const QStringView value = textView();
  if (const auto* shared = std::get_if<SharedTextSlice>(&text_); shared && shared->source == source) {
    const auto range = contentRange();
    if (range.start == shared->start && range.length() == shared->length)
      return true;
  }
  if (value.isEmpty()) {
    return false;
  }
  const QStringView sourceView(*source);
  const auto bindRange = [&](InlineRange range) {
    if (!range.isValid() || range.start > sourceView.size() || range.end > sourceView.size()) {
      return false;
    }
    const QStringView candidate = sourceView.mid(range.start, range.length());
    if (candidate == value) {
      text_ = SharedTextSlice{source, int(range.start), int(range.length())};
      return true;
    }
    const qsizetype relative = candidate.indexOf(value);
    if (relative >= 0) {
      text_ = SharedTextSlice{source, int(range.start + relative), int(value.size())};
      return true;
    }
    return false;
  };
  if (bindRange(contentRange()) || bindRange(sourceRange())) {
    return true;
  }
  return false;
}

void InlineNode::detachSharedText() {
  if (std::holds_alternative<SharedTextSlice>(text_)) {
    text_ = textView().toString();
  }
  for (InlineNode& child : children_) {
    child.detachSharedText();
  }
}

bool InlineNode::setSharedTextFromUtf8(const std::shared_ptr<const QString>& source, InlineRange range,
                                       QUtf8StringView value) {
  if (!source || source->size() > std::numeric_limits<int>::max() || value.isEmpty() || !range.isValid() ||
      range.end > source->size())
    return false;
  const QStringView candidate = QStringView(*source).mid(range.start, range.length());
  if (QAnyStringView::compare(QAnyStringView(candidate), QAnyStringView(value)) != 0)
    return false;
  text_ = SharedTextSlice{source, int(range.start), int(range.length())};
  return true;
}

InlineNode InlineNode::textSegment(qsizetype start, qsizetype length) const {
  Q_ASSERT(start >= 0 && length >= 0 && start <= textView().size() && length <= textView().size() - start);
  InlineNode result(InlineType::Text);
  if (const auto* shared = std::get_if<SharedTextSlice>(&text_)) {
    result.text_ = SharedTextSlice{shared->source, shared->start + int(start), int(length)};
  } else {
    result.text_ = std::get<QString>(text_).mid(start, length);
  }
  return result;
}

bool InlineNode::usesSharedText() const {
  return std::holds_alternative<SharedTextSlice>(text_);
}

QString InlineNode::marker() const {
  const auto& markers = commonMarkers();
  return markerKind_ < std::size(markers) ? markers[markerKind_]
         : extendedMetadata_              ? extendedMetadata_->marker
                                          : QString();
}

void InlineNode::setMarker(QString marker) {
  const auto& markers = commonMarkers();
  for (quint8 i = 0; i < std::size(markers); ++i) {
    if (marker == markers[i]) {
      markerKind_ = i;
      if (extendedMetadata_) {
        extendedMetadata_->marker.clear();
        pruneExtendedMetadata();
      }
      return;
    }
  }
  markerKind_ = 255;
  ensureExtendedMetadata().marker = std::move(marker);
}

QString InlineNode::href() const {
  return extendedMetadata_ ? extendedMetadata_->href : QString();
}

void InlineNode::setHref(QString href) {
  if (href.isEmpty() && !extendedMetadata_) {
    return;
  }
  ensureExtendedMetadata().href = std::move(href);
  pruneExtendedMetadata();
}

QString InlineNode::title() const {
  return extendedMetadata_ ? extendedMetadata_->title : QString();
}

void InlineNode::setTitle(QString title) {
  if (title.isEmpty() && !extendedMetadata_) {
    return;
  }
  ensureExtendedMetadata().title = std::move(title);
  pruneExtendedMetadata();
}

QString InlineNode::alt() const {
  return extendedMetadata_ ? extendedMetadata_->alt : QString();
}

void InlineNode::setAlt(QString alt) {
  if (alt.isEmpty() && !extendedMetadata_) {
    return;
  }
  ensureExtendedMetadata().alt = std::move(alt);
  pruneExtendedMetadata();
}

qsizetype InlineNode::sourceStart() const {
  return sourceRange().start;
}
void InlineNode::setSourceStart(qsizetype start) {
  auto ranges = sourceRanges();
  ranges.source.start = start;
  setSourceRanges(ranges);
}
qsizetype InlineNode::sourceEnd() const {
  return sourceRange().end;
}
void InlineNode::setSourceEnd(qsizetype end) {
  auto ranges = sourceRanges();
  ranges.source.end = end;
  setSourceRanges(ranges);
}
InlineRange InlineNode::sourceRange() const {
  return sourceRanges().source;
}
void InlineNode::setSourceRange(InlineRange range) {
  auto ranges = sourceRanges();
  ranges.source = range;
  setSourceRanges(ranges);
}
InlineRange InlineNode::contentRange() const {
  return sourceRanges().content;
}
void InlineNode::setContentRange(InlineRange range) {
  auto ranges = sourceRanges();
  ranges.content = range;
  setSourceRanges(ranges);
}
InlineRange InlineNode::openMarkerRange() const {
  return sourceRanges().openMarker;
}
void InlineNode::setOpenMarkerRange(InlineRange range) {
  auto ranges = sourceRanges();
  ranges.openMarker = range;
  setSourceRanges(ranges);
}
InlineRange InlineNode::closeMarkerRange() const {
  return sourceRanges().closeMarker;
}
void InlineNode::setCloseMarkerRange(InlineRange range) {
  auto ranges = sourceRanges();
  ranges.closeMarker = range;
  setSourceRanges(ranges);
}
InlineSourceRanges InlineNode::sourceRanges() const {
  if (extendedMetadata_ && extendedMetadata_->wideRanges)
    return *extendedMetadata_->wideRanges;
  return {{sourceRanges_[0], sourceRanges_[1]},
          {sourceRanges_[2], sourceRanges_[3]},
          {sourceRanges_[4], sourceRanges_[5]},
          {sourceRanges_[6], sourceRanges_[7]}};
}
void InlineNode::setSourceRanges(InlineSourceRanges ranges) {
  const std::array<qsizetype, 8> values{ranges.source.start,      ranges.source.end,       ranges.content.start,
                                        ranges.content.end,       ranges.openMarker.start, ranges.openMarker.end,
                                        ranges.closeMarker.start, ranges.closeMarker.end};
  for (const auto value : values) {
    if (value < std::numeric_limits<qint32>::min() || value > std::numeric_limits<qint32>::max()) {
      auto& metadata = ensureExtendedMetadata();
      if (!metadata.wideRanges)
        metadata.wideRanges = std::make_unique<InlineSourceRanges>();
      *metadata.wideRanges = ranges;
      return;
    }
  }
  for (size_t i = 0; i < values.size(); ++i)
    sourceRanges_[i] = qint32(values[i]);
  if (extendedMetadata_ && extendedMetadata_->wideRanges) {
    extendedMetadata_->wideRanges.reset();
    pruneExtendedMetadata();
  }
}

bool InlineNode::isAutolink() const {
  return extendedMetadata_ && extendedMetadata_->autolink;
}

void InlineNode::setAutolink(bool autolink) {
  if (!autolink && !extendedMetadata_) {
    return;
  }
  ensureExtendedMetadata().autolink = autolink;
  pruneExtendedMetadata();
}

QVector<InlineNode>& InlineNode::children() {
  return children_;
}

const QVector<InlineNode>& InlineNode::children() const {
  return children_;
}

InlineNode InlineNode::text(QString value) {
  InlineNode node(InlineType::Text);
  node.setText(std::move(value));
  return node;
}

InlineNode InlineNode::softBreak() {
  return InlineNode(InlineType::SoftBreak);
}

InlineNode InlineNode::lineBreak() {
  return InlineNode(InlineType::LineBreak);
}

InlineNode InlineNode::strong(QString marker, QVector<InlineNode> children) {
  InlineNode node(InlineType::Strong);
  node.setMarker(std::move(marker));
  node.children_ = std::move(children);
  return node;
}

InlineNode InlineNode::emphasis(QString marker, QVector<InlineNode> children) {
  InlineNode node(InlineType::Emphasis);
  node.setMarker(std::move(marker));
  node.children_ = std::move(children);
  return node;
}

InlineNode InlineNode::strikethrough(QString marker, QVector<InlineNode> children) {
  InlineNode node(InlineType::Strikethrough);
  node.setMarker(std::move(marker));
  node.children_ = std::move(children);
  return node;
}

InlineNode InlineNode::highlight(QString marker, QVector<InlineNode> children) {
  InlineNode node(InlineType::Highlight);
  node.setMarker(std::move(marker));
  node.children_ = std::move(children);
  return node;
}

InlineNode InlineNode::subscript(QString marker, QVector<InlineNode> children) {
  InlineNode node(InlineType::Subscript);
  node.setMarker(std::move(marker));
  node.children_ = std::move(children);
  return node;
}

InlineNode InlineNode::superscript(QString marker, QVector<InlineNode> children) {
  InlineNode node(InlineType::Superscript);
  node.setMarker(std::move(marker));
  node.children_ = std::move(children);
  return node;
}

InlineNode InlineNode::code(QString value) {
  InlineNode node(InlineType::Code);
  node.setText(std::move(value));
  return node;
}

InlineNode InlineNode::link(QString href, QString title, QVector<InlineNode> label) {
  InlineNode node(InlineType::Link);
  node.setHref(std::move(href));
  node.setTitle(std::move(title));
  node.children_ = std::move(label);
  return node;
}

InlineNode InlineNode::image(QString src, QString alt, QString title) {
  InlineNode node(InlineType::Image);
  node.setHref(std::move(src));
  node.setAlt(std::move(alt));
  node.setTitle(std::move(title));
  return node;
}

InlineNode InlineNode::inlineMath(QString tex, QString marker) {
  InlineNode node(InlineType::InlineMath);
  node.setText(std::move(tex));
  node.setMarker(std::move(marker));
  return node;
}

InlineNode InlineNode::footnoteReference(QString label, QString ordinal) {
  InlineNode node(InlineType::FootnoteReference);
  node.setText(std::move(ordinal));             // displayed as a superscript link
  node.setMarker(label);                        // bare label, for markdown reconstruction ([^label])
  node.setHref(QStringLiteral("#fn:") + label); // in-document navigation target
  return node;
}

void shiftInlineSourcePositions(QVector<InlineNode>& inlines, qsizetype delta) {
  auto shiftRange = [delta](InlineRange range) {
    if (range.isValid()) {
      range.start += delta;
      range.end += delta;
    }
    return range;
  };
  for (InlineNode& inlineNode : inlines) {
    InlineSourceRanges ranges = inlineNode.sourceRanges();
    ranges.source = shiftRange(ranges.source);
    ranges.content = shiftRange(ranges.content);
    ranges.openMarker = shiftRange(ranges.openMarker);
    ranges.closeMarker = shiftRange(ranges.closeMarker);
    inlineNode.setSourceRanges(ranges);
    shiftInlineSourcePositions(inlineNode.children(), delta);
  }
}

} // namespace muffin
