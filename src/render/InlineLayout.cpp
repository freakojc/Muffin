#include "render/InlineLayout.h"
#include "render/CssFormattingContext.h"
#include "render/CssSizing.h"
#include "render/GeneratedContent.h"
#include "render/RenderMetrics.h"
#include "render/InlineFormatting.h"
#include "theme/CssComputedStyleEngine.h"
#include "theme/CssThemeMapper.h"
#include "theme/CssContent.h"
#include "theme/DocumentStyleTree.h"

#include "document/ImageSyntaxOps.h"
#include "editor/ResourceUrl.h"
#include "render/Blur.h"
#include "render/DecorationPainter.h"
#include "render/GradientPainter.h"
#include "render/ImageLoader.h"
#include "render/ImagePlaceholder.h"
#include "html/HtmlRenderer.h"
#include <QFileInfo>
#include <QTextDocumentFragment>

#include <QPainter>
#include <QPen>
#include <QRegularExpression>
#include <QTextCharFormat>
#include "render/TextLayout.h"
#include <QTextOption>

#include <cmath>

namespace muffin {
QPair<qreal, qreal> InlineLayout::intrinsicWidths() const {
  if (!textLayout_) return {};
  const auto intrinsic = intrinsicTextWidths(*textLayout_, wrapMode_ == QTextOption::NoWrap, anywhereMinimum_);
  return {intrinsic.minContent, intrinsic.maxContent};
}
namespace {

// Use the same object-replacement code point as HTML inline atoms.  Its format
// range supplies the measured math advance while keeping the formula
// indivisible for wrapping, painting and hit testing; the normal space before
// the atom remains the browser-compatible break opportunity.
constexpr QChar kInlineMathPlaceholder(0x00a0);
constexpr QChar kImagePlaceholder(0x2009);  // thin space, distinct from math placeholder
// Reserve atomic boxes with an ink-free glyph, including font fallback paths.
constexpr QChar kGeneratedPseudoPlaceholder(0x00a0);
constexpr QChar kTabIndentSourceChar(0x200b);
constexpr QChar kTabIndentLayoutChar(0x00a0);

QRectF mathUsedBounds(const math::MathLayoutResult& layout) {
  if (!layout.root) return {};
  auto bounds = layout.root->boundsAt({});
  // KaTeX's outer inline CSS box has a fixed strut. Its actual child formatting
  // box can extend above/below that strut (fractions, matrices, rules).
  for (const auto& child : layout.root->children) {
    bounds = bounds.united(child->boundsAt(QPointF(child->xOffset, child->yOffset + child->shift)));
  }
  return bounds;
}

QString flattenPlainText(const QVector<InlineNode>& inlines, bool breakOnSingleNewline, bool& hasHtml) {
  QString text;
  for (const InlineNode& node : inlines) {
    switch (node.type()) {
      case InlineType::Text:
      case InlineType::Code:
      case InlineType::InlineMath:
        text += node.text();
        break;
      case InlineType::HtmlInline:
        hasHtml = true;
        // <br> is a line break in plain text; other inline HTML keeps its literal text.
        if (isStandaloneBrTag(node.text())) {
          text += QLatin1Char('\n');
        } else {
          text += node.text();
        }
        break;
      case InlineType::SoftBreak:
        text += breakOnSingleNewline ? QLatin1Char('\n') : QLatin1Char(' ');
        break;
      case InlineType::LineBreak:
        text += QLatin1Char('\n');
        break;
      case InlineType::Image:
        text += node.alt();
        break;
      default:
        text += flattenPlainText(node.children(), breakOnSingleNewline, hasHtml);
        break;
    }
  }
  return text;
}

QString layoutTextForDisplayText(QString text) {
  text.replace(kTabIndentSourceChar, kTabIndentLayoutChar);
  // This Qt build's TextLayout createLine() treats only U+2028 (Line Separator) as a hard
  // break, not '\n' (0x0a) — so a '\n' in the display text (a <br> or a Markdown hard break)
  // would not wrap. Convert '\n' to U+2028 for layout only. Length-preserving (1:1), so every
  // offset/coordinate computed from displayMap_.text() stays valid; displayMap_.text() keeps the '\n'.
  text.replace(QLatin1Char('\n'), QChar(0x2028));
  return text;
}

bool isImageSourceRevealed(const QVector<InlineProjectionSpan>& spans, const InlineProjectionSpan& imageSpan) {
  for (const InlineProjectionSpan& span : spans) {
    if (span.type != InlineType::Image ||
        (span.kind != InlineSpanKind::OpenMarker && span.kind != InlineSpanKind::HiddenSyntax)) {
      continue;
    }
    if (span.sourceStart >= imageSpan.sourceStart && span.sourceEnd <= imageSpan.sourceEnd) {
      return true;
    }
  }
  return false;
}

// Linear RGB-A blend of two opaque theme colours, used to animate a `:hover { color }`
// between the element base colour and the hover target as the HoverAnimator phase ramps.
QColor lerpColor(const QColor& from, const QColor& to, qreal t) {
  t = qBound(0.0, t, 1.0);
  return QColor(qRound(from.red() + (to.red() - from.red()) * t),
                qRound(from.green() + (to.green() - from.green()) * t),
                qRound(from.blue() + (to.blue() - from.blue()) * t),
                qRound(from.alpha() + (to.alpha() - from.alpha()) * t));
}

}  // namespace

struct InlineLayout::TextLayoutPointHit {
  qsizetype displayOffset = 0;
  InlineProjectionBias bias = InlineProjectionBias::Backward;
  QRectF cursorRect;
};

void InlineLayout::build(
    const QVector<InlineNode>& inlines,
    const RenderTheme& theme,
    qreal width,
    const QFont& baseFont) {
  build(inlines, theme, width, baseFont, BuildOptions{});
}

void InlineLayout::build(
    const QVector<InlineNode>& inlines,
    const RenderTheme& theme,
    qreal width,
    const QFont& baseFont,
    BuildOptions options) {
  build(inlines, InlineProjection::markdownForInlines(inlines), theme, width, baseFont, options);
}

void InlineLayout::build(
    const QVector<InlineNode>& inlines,
    QString sourceText,
    const RenderTheme& theme,
    qreal width,
    const QFont& baseFont,
    BuildOptions options) {
  hasHtmlContent_ = false;
  plainText_ = flattenPlainText(inlines, options.breakOnSingleNewline, hasHtmlContent_);
  mathAtoms_.clear();
  imageAtoms_.clear();
  htmlAtoms_.clear();
  previewAtoms_.clear();
  previewHeight_ = 0.0;
  htmlFormatSpans_.clear();
  generatedPseudoAtoms_.clear();
  generatedPseudoFormats_.clear();
  inlinePseudoFingerprints_.clear();
  displayMap_ = {};
  composition_ = {};
  lineBoxes_.clear();
  isMisspelled_ = options.isMisspelled;
  const auto sharedSnapshot = [&](const ThemeElementStyle& style, const QFont& font) {
    const auto used = theme.usedBoxForStyle(style, width);
    return options.styleCache ? options.styleCache->snapshot(style, used, font, width)
                              : std::make_shared<const LayoutBox>(LayoutBox::place(style.key, style, used, {}, font));
  };
  const auto snapshot = [&](const QString& key) {
    const auto* style = options.styleNode ? theme.elementStyleForNode(*options.styleNode, key) : theme.elementStyle(key);
    return sharedSnapshot(style ? *style : ThemeElementStyle{}, theme.textFontForElement(key, options.styleNode));
  };
  codeStyle_ = snapshot(QStringLiteral("code"));
  markGradient_ = GradientSpec{};
  for (const ElementBackground& eb : theme.decorations().backgrounds) {
    if (eb.host == QStringLiteral("mark")) { markGradient_ = eb.gradient; }
  }
  baseTextColorOverride_ = options.baseTextColor;
  hoverTextColor_ = options.hoverTextColor;
  focusTextColor_ = options.focusTextColor;
  preeditText_ = options.preeditText;
  preeditFormats_ = options.preeditFormats;
  preeditCursor_ = options.preeditCursor;
  preeditInsertAtSourceOffset_ = options.preeditInsertAtSourceOffset;
  lineHeightMultiplier_ = options.lineHeightMultiplier;
  zoomScale_ = theme.zoomPercent() / 100.0;
  textShadow_ = options.textShadow;
  alignment_ = options.alignment;
  wrapMode_ = options.wrapMode;
  anywhereMinimum_ = options.anywhereMinimum;
  html::HtmlColorPalette htmlPalette = html::HtmlColorPalette::defaultLight();
  htmlPalette.documentStyleSheet = theme.documentStyleSheet();
  if (options.styleNode) {
    htmlPalette.documentParent = theme.cssElementForNode(*options.styleNode);
    htmlPalette.documentParentForOffset = [&theme, owner = options.styleNode, base = qMax<qsizetype>(0, options.sourceBase)](
                                              qsizetype offset) { return theme.cssParentForInlineHtml(*owner, offset + base); };
  }
  htmlPalette.fontAliases = theme.fontAliases();
  htmlPalette.cssEnvironment = theme.documentCssEnvironment();
  htmlPalette.cssZoom = theme.zoomPercent() / 100.0;
  htmlPalette.parentFontPx = baseFont.pointSizeF() * 96.0 / 72.0 / htmlPalette.cssZoom / htmlPalette.cssEnvironment.textScale;
  htmlPalette.text = options.baseTextColor.isValid() ? options.baseTextColor : theme.textColor();
  projection_ = InlineProjection(inlines, std::move(sourceText), options.projectionState, options.sourceBase, baseFont.pointSizeF(),
                                 options.pendingPrefixLength, options.smartPunct, options.breakOnSingleNewline, options.textTransform,
                                 options.renderEmoji, std::move(htmlPalette));
  spanStyles_.clear();
  styleSourceBase_ = qMax<qsizetype>(0, options.sourceBase);
  if (options.styleNode) {
    for (const auto& span : projection_.spans()) {
      if (span.kind != InlineSpanKind::Text && span.type != InlineType::Image) continue;
      const auto style = theme.inlineStyleForNode(*options.styleNode, span.contentSourceStart + qMax<qsizetype>(0, options.sourceBase));
      const QFont fallback = span.type == InlineType::Code ? codeStyle_->font : baseFont;
      spanStyles_.insert(span.displayStart, sharedSnapshot(style, theme.fontForStyle(style, fallback)));
    }
  }
  buildOffsetMapFromProjection();
  buildMathAtoms(inlines, theme, width);
  buildImageAtoms(inlines, theme, width, options.documentPath);
  buildHtmlAtoms(width, baseFont, options.documentPath);
  buildHtmlFormatSpans(theme, width);
  buildInlineBoxSpacing();
  buildGeneratedPseudoAtoms(theme, baseFont, width, options);
  buildHtmlFormatSpans(theme, width);
  buildTextLayout(theme, width, baseFont);
  buildInlineBoxes();
  // Genuinely empty: no text glyphs and no rendered image. Checking the image
  // atoms (not just plainText) matters because an image with blank alt text
  // flattens to empty text but is still visible content.
  isEmpty_ = plainText_.isEmpty() && imageAtoms_.isEmpty() && htmlAtoms_.isEmpty() && generatedPseudoAtoms_.isEmpty();
}

QSizeF InlineLayout::size() const {
  return size_;
}

bool InlineLayout::stylesMatch(const RenderTheme& theme, const MarkdownNode& owner) const {
  const auto fingerprint = [&](const QString& pseudo) {
    const auto rule = theme.pseudoForNode(owner, pseudo);
    return rule && rule->computed ? rule->computed->fingerprint() : quint64(0);
  };
  if (fingerprint(QStringLiteral("before")) != pseudoBeforeFingerprint_ ||
      fingerprint(QStringLiteral("after")) != pseudoAfterFingerprint_) return false;
  for (const auto& stored : inlinePseudoFingerprints_) {
    const auto* origin = theme.cssInlineElement(owner, stored.sourceOffset, stored.tag);
    const auto rule = origin ? theme.pseudoForElement(*origin, stored.pseudo) : std::nullopt;
    if ((rule && rule->computed ? rule->computed->fingerprint() : quint64(0)) != stored.fingerprint) return false;
  }
  // Embedded HTML has its own descendant tree. Until that tree is retained,
  // conservatively rebuild promoted HTML fragments after structural edits.
  if (!projection_.htmlFormatData().isEmpty()) return false;
  for (const auto& span : projection_.spans()) {
    const auto found = spanStyles_.constFind(span.displayStart);
    if (found != spanStyles_.cend() &&
        theme.inlineStyleForNode(owner, span.contentSourceStart + styleSourceBase_).fingerprint != found.value()->style.fingerprint)
      return false;
  }
  return true;
}

qreal InlineLayout::height() const {
  return size_.height();
}

QRectF InlineLayout::visualTextBounds() const {
  if (!textLayout_ || textLayout_->lineCount() == 0) {
    return QRectF();
  }
  QRectF bounds;
  bool have = false;
  for (int i = 0; i < textLayout_->lineCount(); ++i) {
    const TextLine line = textLayout_->lineAt(i);
    if (!line.isValid()) { continue; }
    const int start = line.textStart();
    const int end = start + line.textLength();
    const qreal x1 = line.cursorToX(start);
    const qreal x2 = line.cursorToX(end);
    const QRectF lineRect(qMin(x1, x2), line.y(), qAbs(x2 - x1), line.height());
    bounds = have ? bounds.united(lineRect) : lineRect;
    have = true;
  }
  for (const auto& box : inlineBoxes_) bounds = bounds.united(box.visualOverflow);
  for (const auto& rect : mathAtomRects({})) bounds = bounds.united(rect);
  for (const auto& atom : htmlAtoms_) bounds = bounds.united(atom.rect);
  for (const auto& atom : generatedPseudoAtoms_)
    if (atom.kind != GeneratedPseudoAtom::Kind::Text) bounds = bounds.united(atom.box.visualOverflow);
  return bounds;
}

qreal InlineLayout::firstLineBaselineY() const {
  if (!textLayout_ || textLayout_->lineCount() == 0) {
    return 0.0;
  }
  const TextLine line = textLayout_->lineAt(0);
  // line.y() is the centering offset applied for line-height (see buildTextLayout);
  // line.ascent() is the font ascent. Their sum is where the first line's text
  // baseline sits relative to the layout origin.
  return line.isValid() ? (line.y() + line.ascent()) : 0.0;
}

qreal InlineLayout::lastLineBaselineY() const {
  if (!textLayout_ || textLayout_->lineCount() == 0) return 0;
  const auto line = textLayout_->lineAt(textLayout_->lineCount() - 1);
  return line.y() + line.ascent();
}

void InlineLayout::paint(QPainter& painter, QPointF origin, qreal hoverPhase, qreal focusPhase) const {
  if (!textLayout_) {
    return;
  }

  painter.save();
  paintTextLayoutHtmlBackgrounds(painter, origin);
  paintTextLayoutCodeSpans(painter, origin);
  paintTextLayoutInlineDecorations(painter, origin);
  paintTextLayoutHtmlKeyboardSpans(painter, origin);
  // CSS `text-shadow`: render the laid-out text to an offscreen image, recolour it
  // to the shadow colour (alpha mask), blur, and composite behind the crisp text.
  if (textShadow_.present && textShadow_.color.isValid() && textLayout_) {
    paintTextShadow(painter, origin);
  }
  // CSS `:hover`/`:focus { color }` recolours only the runs that inherit the element
  // colour (pre-computed in hoverRecolourRanges_), animated by the HoverAnimator/
  // FocusAnimator phases. Applied as foreground-only `selection`s passed to draw() —
  // a draw-time override of the foreground that needs NO setFormats() (broken after
  // endLayout in this Qt build) and NO second render, so glyph edges stay crisp and
  // styled spans (links/code/del/kbd) keep their own colours. The selection replaces
  // (not blends) the per-run colour, so the target is the lerped colour: focus is
  // applied first (base→focus), then hover on top (→hover) — the two orthogonal
  // states compose, with hover visually winning when both are at full phase.
  const bool hasFocus = focusTextColor_.isValid() && focusPhase > 0.0;
  const bool hasHover = hoverTextColor_.isValid() && hoverPhase > 0.0;
  if ((hasFocus || hasHover) && !hoverRecolourRanges_.isEmpty()) {
    QColor target = baseRunColor_;
    if (hasFocus) { target = lerpColor(target, focusTextColor_, qBound(0.0, focusPhase, 1.0)); }
    if (hasHover) { target = lerpColor(target, hoverTextColor_, qBound(0.0, hoverPhase, 1.0)); }
    QVector<QTextLayout::FormatRange> selections;
    selections.reserve(hoverRecolourRanges_.size());
    for (const QPair<int, int>& range : hoverRecolourRanges_) {
      QTextCharFormat format;
      format.setForeground(target);
      QTextLayout::FormatRange selection;
      selection.start = range.first;
      selection.length = range.second - range.first;
      selection.format = format;
      selections.append(selection);
    }
    textLayout_->draw(&painter, origin, selections);
  } else {
    textLayout_->draw(&painter, origin);
  }
  paintTextLayoutMathAtoms(painter, origin);
  paintTextLayoutImageAtoms(painter, origin);
  for (const auto& atom : htmlAtoms_)
    atom.layout->paintBoxFragment(painter, *atom.layout->root()->children().front(), origin + atom.rect.topLeft() - atom.crop.topLeft());
  paintGeneratedPseudoAtoms(painter, origin);
  paintImagePreview(painter, origin);
  painter.restore();
}

qsizetype InlineLayout::hitTestTextOffset(QPointF localPos) const {
  for (const auto& atom : htmlAtoms_)
    if (atom.rect.contains(localPos))
      return atom.visibleStart + atom.layout->textOffsetAtPoint(localPos - atom.rect.topLeft() + atom.crop.topLeft());
  return visibleOffsetForDisplayOffset(textLayoutDisplayOffsetForPoint(localPos));
}

qsizetype InlineLayout::hitTestSourceOffset(QPointF localPos) const {
  for (const auto& atom : htmlAtoms_)
    if (atom.rect.contains(localPos)) {
      const auto offset = atom.layout->textOffsetAtPoint(localPos - atom.rect.topLeft() + atom.crop.topLeft());
      return atom.sourceOffsets.value(offset, atom.sourceEnd);
    }
  const TextLayoutPointHit hit = textLayoutHitForPoint(localPos);
  const qsizetype position = hit.displayOffset;
  for (const MathAtom& atom : mathAtoms_) {
    if (position < atom.displayStart || position > atom.displayEnd) {
      continue;
    }
    qreal atomLeft = textLayoutCursorRectForDisplayOffset(atom.displayStart).left();
    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const TextLine line = textLayout_->lineAt(i);
      if (!line.isValid()) {
        continue;
      }
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      const int atomStart = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atomStart >= lineStart && atomStart <= lineEnd) {
        atomLeft = line.cursorToX(atomStart);
        break;
      }
    }
    const qreal atomWidth = atom.layout && atom.layout->valid() ? atom.layout->size.width() : 0.0;
    if (atomWidth <= 0.0 || atom.contentSourceEnd <= atom.contentSourceStart) {
      return localPos.x() > atomLeft ? atom.sourceEnd : atom.sourceStart;
    }
    const qreal ratio = qBound<qreal>(0.0, (localPos.x() - atomLeft) / atomWidth, 1.0);
    const qsizetype contentLength = atom.contentSourceEnd - atom.contentSourceStart;
    const qsizetype contentOffset = static_cast<qsizetype>(qRound(ratio * contentLength));
    return atom.contentSourceStart + qBound<qsizetype>(0, contentOffset, contentLength);
  }
  const qsizetype projPosition = projectionDisplayOffsetForLayoutOffset(position, hit.bias);
  for (const InlineProjectionSpan& span : projection_.spans()) {
    if ((span.type != InlineType::InlineMath && span.type != InlineType::Code) ||
        span.kind != InlineSpanKind::OpenMarker ||
        span.displayEnd <= span.displayStart ||
        projPosition < span.displayStart ||
        projPosition > span.displayEnd) {
      continue;
    }
    const QRectF markerStart = textLayoutCursorRectForDisplayOffset(span.displayStart);
    const QRectF markerEnd = textLayoutCursorRectForDisplayOffset(span.displayEnd);
    const qreal markerLeft = qMin(markerStart.left(), markerEnd.left());
    const qreal markerRight = qMax(markerStart.left(), markerEnd.left());
    if (localPos.x() >= markerLeft && localPos.x() <= markerRight) {
      return span.sourceEnd;
    }
  }
  qsizetype sourceOffset = -1;
  if (projection_.sourceOffsetForDisplayOffset(projPosition, hit.bias, sourceOffset)) {
    return sourceOffset;
  }
  return visibleOffsetForDisplayOffset(position);
}

QRectF InlineLayout::hitTestCursorRect(QPointF localPos) const {
  for (const auto& atom : htmlAtoms_)
    if (atom.rect.contains(localPos)) {
      const auto point = localPos - atom.rect.topLeft() + atom.crop.topLeft();
      return atom.layout->cursorRectForTextOffset(atom.layout->textOffsetAtPoint(point))
          .translated(atom.rect.topLeft() - atom.crop.topLeft());
    }
  const auto hit = textLayoutHitForPoint(localPos);
  const auto display = hit.displayOffset;
  for (const auto& atom : generatedPseudoAtoms_)
    if (display >= atom.rangeStart && display <= atom.rangeEnd)
      return cursorRect(atom.visibleAnchor);
  return hit.cursorRect;
}

QString InlineLayout::linkHrefAtLocalPos(QPointF localPos) const {
  for (const auto& atom : htmlAtoms_)
    if (atom.rect.contains(localPos)) return atom.layout->hitTest(localPos - atom.rect.topLeft() + atom.crop.topLeft()).linkHref;
  if (!textLayout_) return {};
  // A click in the right half of the last generated glyph can resolve to its
  // ending caret. Use the painted fragment geometry for the link target.
  for (const auto& atom : generatedPseudoAtoms_) {
    if (atom.href.isEmpty()) continue;
    if (atom.kind != GeneratedPseudoAtom::Kind::Text) {
      if (atom.box.borderBox.contains(localPos)) return atom.href;
    } else for (const auto& rect : selectionRectsForDisplayOffsets(atom.displayStart, atom.displayEnd))
      if (rect.contains(localPos)) return atom.href;
  }
  const qsizetype layoutOffset = textLayoutDisplayOffsetForPoint(localPos);
  for (const auto& atom : generatedPseudoAtoms_)
    if (layoutOffset >= atom.rangeStart && layoutOffset < atom.rangeEnd && !atom.href.isEmpty()) return atom.href;
  const qsizetype projOffset = projectionDisplayOffsetForLayoutOffset(layoutOffset, InlineProjectionBias::Backward);
  return projection_.linkHrefAtDisplayOffset(projOffset);
}

QString InlineLayout::imageSrcAtLocalPos(QPointF localPos) const {
  for (const auto& atom : htmlAtoms_)
    if (atom.rect.contains(localPos)) return atom.layout->hitTest(localPos - atom.rect.topLeft() + atom.crop.topLeft()).imageSrc;
  if (!textLayout_) return {};
  const qsizetype position = textLayoutDisplayOffsetForPoint(localPos);
  for (const ImageAtom& atom : imageAtoms_) {
    if (position >= atom.displayStart && position <= atom.displayEnd) {
      return atom.srcUrl;
    }
  }
  return {};
}

QRectF InlineLayout::cursorRect(qsizetype textOffset) const {
  for (const auto& atom : htmlAtoms_)
    if (textOffset > atom.visibleStart && textOffset < atom.visibleEnd)
      return atom.layout->cursorRectForTextOffset(textOffset - atom.visibleStart).translated(atom.rect.topLeft() - atom.crop.topLeft());
  for (const MathAtom& atom : mathAtoms_) {
    if (textOffset > atom.visibleStart && textOffset < atom.visibleEnd) {
      textOffset = textOffset - atom.visibleStart < atom.visibleEnd - textOffset ? atom.visibleStart : atom.visibleEnd;
      break;
    }
  }
  for (const ImageAtom& atom : imageAtoms_) {
    if (textOffset > atom.visibleStart && textOffset < atom.visibleEnd) {
      textOffset = textOffset - atom.visibleStart < atom.visibleEnd - textOffset ? atom.visibleStart : atom.visibleEnd;
      break;
    }
  }
  return textLayoutCursorRectForDisplayOffset(displayOffsetForVisibleOffset(textOffset));
}

QRectF InlineLayout::cursorRectForSourceOffset(qsizetype sourceOffset) const {
  for (const auto& atom : htmlAtoms_)
    if (sourceOffset > atom.sourceStart && sourceOffset < atom.sourceEnd) {
      const auto found = std::lower_bound(atom.sourceOffsets.begin(), atom.sourceOffsets.end(), sourceOffset);
      const auto offset = int(found - atom.sourceOffsets.begin());
      const auto rect = atom.layout->cursorRectForTextOffset(offset);
      if (!rect.isNull()) return rect.translated(atom.rect.topLeft() - atom.crop.topLeft());
    }
  for (const MathAtom& atom : mathAtoms_) {
    if (sourceOffset > atom.sourceStart && sourceOffset < atom.sourceEnd) {
      const qsizetype displayOffset = sourceOffset - atom.sourceStart < atom.sourceEnd - sourceOffset ? atom.displayStart : atom.displayEnd;
      return textLayoutCursorRectForDisplayOffset(displayOffset);
    }
  }
  for (const ImageAtom& atom : imageAtoms_) {
    if (sourceOffset > atom.sourceStart && sourceOffset < atom.sourceEnd) {
      const qsizetype displayOffset = sourceOffset - atom.sourceStart < atom.sourceEnd - sourceOffset ? atom.displayStart : atom.displayEnd;
      return textLayoutCursorRectForDisplayOffset(displayOffset);
    }
  }
  qsizetype displayOffset = -1;
  if (!layoutDisplayOffsetForSourceOffset(sourceOffset, InlineProjectionBias::Forward, displayOffset)) {
    return cursorRect(sourceOffset);
  }
  return textLayoutCursorRectForDisplayOffset(displayOffset);
}

int InlineLayout::visualLineCount() const {
  return textLayout_ ? textLayout_->lineCount() : 0;
}

int InlineLayout::visualLineIndexForTextOffset(qsizetype textOffset) const {
  if (!textLayout_) {
    return -1;
  }
  const int displayOffset = toLayoutOffset(static_cast<int>(displayOffsetForVisibleOffset(textOffset)));
  for (int i = 0; i < textLayout_->lineCount(); ++i) {
    const TextLine line = textLayout_->lineAt(i);
    if (!line.isValid()) { continue; }
    const int lineStart = line.textStart();
    const int lineEnd = lineStart + line.textLength();
    if (displayOffset >= lineStart && displayOffset <= lineEnd) {
      return i;
    }
  }
  return textLayout_->lineCount() > 0 ? textLayout_->lineCount() - 1 : -1;
}

int InlineLayout::visualLineIndexForSourceOffset(qsizetype sourceOffset) const {
  if (!textLayout_) {
    return -1;
  }
  qsizetype displayOffset = -1;
  if (!layoutDisplayOffsetForSourceOffset(sourceOffset, InlineProjectionBias::Forward, displayOffset)) {
    return visualLineIndexForTextOffset(sourceOffset);
  }
  displayOffset = toLayoutOffset(static_cast<int>(displayOffset));
  for (int i = 0; i < textLayout_->lineCount(); ++i) {
    const TextLine line = textLayout_->lineAt(i);
    if (!line.isValid()) { continue; }
    const int lineStart = line.textStart();
    const int lineEnd = lineStart + line.textLength();
    if (displayOffset >= lineStart && displayOffset <= lineEnd) {
      return i;
    }
  }
  return textLayout_->lineCount() > 0 ? textLayout_->lineCount() - 1 : -1;
}

QRectF InlineLayout::visualLineRect(int lineIndex) const {
  if (!textLayout_ || lineIndex < 0 || lineIndex >= textLayout_->lineCount()) {
    return {};
  }
  const TextLine line = textLayout_->lineAt(lineIndex);
  if (!line.isValid()) {
    return {};
  }
  const int lineStart = line.textStart();
  const int lineEnd = lineStart + line.textLength();
  const qreal x1 = line.cursorToX(lineStart);
  const qreal x2 = line.cursorToX(lineEnd);
  return QRectF(qMin(x1, x2), line.y(), qAbs(x2 - x1), line.height());
}

qsizetype InlineLayout::textOffsetAtVisualLineX(int lineIndex, qreal localX) const {
  if (!textLayout_ || lineIndex < 0 || lineIndex >= textLayout_->lineCount()) {
    return 0;
  }
  const TextLine line = textLayout_->lineAt(lineIndex);
  if (!line.isValid()) {
    return 0;
  }
  return hitTestTextOffset(QPointF(localX, line.y() + line.height() * 0.5));
}

qsizetype InlineLayout::sourceOffsetAtVisualLineX(int lineIndex, qreal localX) const {
  if (!textLayout_ || lineIndex < 0 || lineIndex >= textLayout_->lineCount()) {
    return 0;
  }
  const TextLine line = textLayout_->lineAt(lineIndex);
  if (!line.isValid()) {
    return 0;
  }
  return hitTestSourceOffset(QPointF(localX, line.y() + line.height() * 0.5));
}

int InlineLayout::toLayoutOffset(int displayOffset) const { return composition_.toLayoutOffset(displayOffset); }

QRectF InlineLayout::preeditCursorRect(QPointF origin) const {
  if (!textLayout_ || !composition_.active() || preeditCursor_ < 0) {
    return {};
  }
  const int pos = composition_.cursorOffset(preeditCursor_);
  for (int i = 0; i < textLayout_->lineCount(); ++i) {
    const TextLine line = textLayout_->lineAt(i);
    if (line.isValid() && pos >= line.textStart() && pos <= line.textStart() + line.textLength()) {
      return QRectF(origin.x() + line.cursorToX(pos), origin.y() + line.y(), 1.5, line.height());
    }
  }
  return {};
}

QVector<QRectF> InlineLayout::mathAtomRects(QPointF origin) const {
  QVector<QRectF> rects;
  if (!textLayout_) {
    return rects;
  }
  for (const MathAtom& atom : mathAtoms_) {
    if (!atom.layout || !atom.layout->valid()) {
      continue;
    }
    const int atomStart = toLayoutOffset(static_cast<int>(atom.displayStart));
    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const TextLine line = textLayout_->lineAt(i);
      if (!line.isValid()) {
        continue;
      }
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      if (atomStart >= lineStart && atomStart < lineEnd) {
        const qreal x = line.cursorToX(atomStart);
        const qreal baseline = origin.y() + line.y() + line.ascent();
        const auto used = mathUsedBounds(*atom.layout);
        rects.append(QRectF(origin.x() + x, baseline + used.top(), atom.layout->size.width(), used.height()));
        break;
      }
    }
  }
  return rects;
}

QVector<QRectF> InlineLayout::selectionRects(qsizetype startOffset, qsizetype endOffset) const {
  const qsizetype startDisplayOffset = displayOffsetForVisibleOffset(qMin(startOffset, endOffset));
  const qsizetype endDisplayOffset = displayOffsetForVisibleOffset(qMax(startOffset, endOffset));
  auto rects = selectionRectsForDisplayOffsets(startDisplayOffset, endDisplayOffset);
  for (const auto& atom : htmlAtoms_) {
    if (qMax(startOffset, endOffset) <= atom.visibleStart || qMin(startOffset, endOffset) >= atom.visibleEnd) continue;
    for (const auto& rect : atom.layout->selectionRects(qMax<qsizetype>(0, qMin(startOffset, endOffset) - atom.visibleStart),
                                                        qMin(atom.visibleEnd, qMax(startOffset, endOffset)) - atom.visibleStart))
      rects.push_back(rect.translated(atom.rect.topLeft() - atom.crop.topLeft()));
  }
  return rects;
}

QVector<QRectF> InlineLayout::selectionRectsForSourceOffsets(qsizetype startSourceOffset, qsizetype endSourceOffset) const {
  qsizetype startDisplayOffset = -1;
  qsizetype endDisplayOffset = -1;
  if (!layoutDisplayOffsetForSourceOffset(qMin(startSourceOffset, endSourceOffset), InlineProjectionBias::Backward, startDisplayOffset) ||
      !layoutDisplayOffsetForSourceOffset(qMax(startSourceOffset, endSourceOffset), InlineProjectionBias::Forward, endDisplayOffset)) {
    return {};
  }
  auto rects = selectionRectsForDisplayOffsets(startDisplayOffset, endDisplayOffset);
  for (const auto& atom : htmlAtoms_) {
    const auto first = qMin(startSourceOffset, endSourceOffset), last = qMax(startSourceOffset, endSourceOffset);
    if (last <= atom.sourceStart || first >= atom.sourceEnd) continue;
    if (first <= atom.sourceStart && last >= atom.sourceEnd)
      rects.push_back(atom.rect);
    else {
      const auto begin = std::lower_bound(atom.sourceOffsets.begin(), atom.sourceOffsets.end(), first) - atom.sourceOffsets.begin();
      const auto end = std::lower_bound(atom.sourceOffsets.begin(), atom.sourceOffsets.end(), last) - atom.sourceOffsets.begin();
      for (const auto& rect : atom.layout->selectionRects(begin, end))
        rects.push_back(rect.translated(atom.rect.topLeft() - atom.crop.topLeft()));
    }
  }
  return rects;
}

QVector<QRectF> InlineLayout::selectionRectsForDisplayOffsets(qsizetype startDisplayOffset, qsizetype endDisplayOffset) const {
  QVector<QRectF> rects;
  if (!textLayout_) {
    return rects;
  }

  const int start = toLayoutOffset(qBound(0, static_cast<int>(qMin(startDisplayOffset, endDisplayOffset)), static_cast<int>(displayMap_.text().size())));
  const int end = toLayoutOffset(qBound(0, static_cast<int>(qMax(startDisplayOffset, endDisplayOffset)), static_cast<int>(displayMap_.text().size())));
  if (start == end) {
    return rects;
  }

  for (int i = 0; i < textLayout_->lineCount(); ++i) {
    const TextLine line = textLayout_->lineAt(i);
    if (!line.isValid()) {
      continue;
    }
    const int lineStart = line.textStart();
    const int lineEnd = lineStart + line.textLength();
    const int rangeStart = qMax(start, lineStart);
    const int rangeEnd = qMin(end, lineEnd);
    if (rangeStart >= rangeEnd) {
      continue;
    }
    const qreal x1 = line.cursorToX(rangeStart);
    const qreal x2 = line.cursorToX(rangeEnd);
    rects.push_back(QRectF(qMin(x1, x2), line.y(), qAbs(x2 - x1), line.height()));
  }
  return rects;
}

void InlineLayout::paintTextLayoutCodeSpans(QPainter& painter, QPointF origin) const {
  for (const auto& box : inlineBoxes_)
    if (box.hostKey == QStringLiteral("code") || box.hostKey.contains(QStringLiteral("::"))) paintLayoutBox(painter, box, origin);
}

QString InlineLayout::plainText() const { return plainText_; }
QString InlineLayout::visibleText() const { return projection_.visibleText(); }
QString InlineLayout::displayText() const { return displayMap_.text(); }
int InlineLayout::mathAtomCount() const { return static_cast<int>(mathAtoms_.size()); }
QVector<QTextLayout::FormatRange> InlineLayout::debugTextFormats(const RenderTheme& theme, const QFont& baseFont) const {
  return textLayoutFormats(theme, baseFont, shapingText());
}

void InlineLayout::paintTextLayoutInlineDecorations(QPainter& painter, QPointF origin) const {
  if (!textLayout_) { return; }
  const bool hasMark = markGradient_.kind != GradientSpec::Kind::None;
  if (!hasMark) return;

  // mark background-image gradient, per occupied line (mirrors code-span rects).
  if (hasMark) {
    painter.save();
    for (const InlineProjectionSpan& span : projection_.spans()) {
      if (!span.highlight || span.kind != InlineSpanKind::Text || span.displayEnd <= span.displayStart) { continue; }
      for (int i = 0; i < textLayout_->lineCount(); ++i) {
        const TextLine line = textLayout_->lineAt(i);
        if (!line.isValid()) { continue; }
        const int lineStart = line.textStart();
        const int lineEnd = lineStart + line.textLength();
        const DisplayOffsetRange lr = layoutDisplayRangeForProjectionRange(span.displayStart, span.displayEnd);
        if (!lr.valid) { continue; }
        const int rs = qMax(lineStart, toLayoutOffset(static_cast<int>(lr.start)));
        const int re = qMin(lineEnd, toLayoutOffset(static_cast<int>(lr.end)));
        if (rs >= re) { continue; }
        const qreal x1 = line.cursorToX(rs);
        const qreal x2 = line.cursorToX(re);
        const QRectF rect(origin.x() + qMin(x1, x2), origin.y() + line.y(), qAbs(x2 - x1), line.height());
        painter.fillRect(rect, GradientPainter::makeBrush(markGradient_, rect, zoomScale_));
      }
    }
    painter.restore();
  }

}

void InlineLayout::paintTextLayoutHtmlBackgrounds(QPainter& painter, QPointF origin) const {
  if (!textLayout_ || htmlFormatSpans_.isEmpty()) {
    return;
  }

  painter.save();
  for (const HtmlFormatSpan& hs : htmlFormatSpans_) {
    if (!hs.backgroundColor.isValid() || hs.backgroundColor.alpha() == 0 || hs.layoutEnd <= hs.layoutStart) {
      continue;
    }

    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const TextLine line = textLayout_->lineAt(i);
      if (!line.isValid()) {
        continue;
      }
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      const int rangeStart = qMax(lineStart, toLayoutOffset(hs.layoutStart));
      const int rangeEnd = qMin(lineEnd, toLayoutOffset(hs.layoutEnd));
      if (rangeStart >= rangeEnd) {
        continue;
      }
      const qreal x1 = line.cursorToX(rangeStart);
      const qreal x2 = line.cursorToX(rangeEnd);
      const QRectF rect(
          origin.x() + qMin(x1, x2) - 1.0,
          origin.y() + line.y() + 1.0,
          qAbs(x2 - x1) + 2.0,
          qMax<qreal>(1.0, line.height() - 2.0));
      painter.setPen(Qt::NoPen);
      painter.setBrush(hs.backgroundColor);
      painter.drawRoundedRect(rect, 2.0, 2.0);
    }
  }
  painter.restore();
}

void InlineLayout::paintTextLayoutHtmlKeyboardSpans(QPainter& painter, QPointF origin) const {
  for (const auto& box : inlineBoxes_)
    if (box.hostKey == QStringLiteral("kbd")) paintLayoutBox(painter, box, origin);
}

void InlineLayout::buildInlineBoxes() {
  inlineBoxes_.clear();
  if (!textLayout_) return;
  const auto add = [&](qsizetype start, qsizetype end, const LayoutBox& style) {
    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const auto line = textLayout_->lineAt(i);
      const int first = qMax(line.textStart(), toLayoutOffset(start));
      const int last = qMin(line.textStart() + line.textLength(), toLayoutOffset(end));
      if (first >= last) continue;
      const qreal left = line.cursorToX(first), right = line.cursorToX(last);
      inlineBoxes_.push_back(LayoutBox::inlineFragment(style, left, right, line.y() + line.ascent(),
                                                       first == toLayoutOffset(start), last == toLayoutOffset(end)));
    }
  };
  for (const auto& span : projection_.spans()) {
    if (span.type != InlineType::Code || span.kind != InlineSpanKind::Text) continue;
    const auto range = layoutDisplayRangeForProjectionRange(span.displayStart, span.displayEnd);
    if (range.valid) add(range.start, range.end, *spanStyles_.value(span.displayStart, codeStyle_));
  }
  for (const auto& run : htmlInlineBoxRuns_) add(run.start, run.end, run.box);
  for (const auto& atom : generatedPseudoAtoms_)
    if (atom.kind == GeneratedPseudoAtom::Kind::Text && atom.box.valid)
      add(atom.displayStart, atom.displayEnd, atom.box);
}

void InlineLayout::buildOffsetMapFromProjection() { displayMap_.reset(projection_); }

void InlineLayout::buildInlineBoxSpacing() {
  inlineSpacers_.clear();
  struct Edge {
    qsizetype position;
    qreal width;
  };
  QVector<Edge> edges;
  const auto addBox = [&](qsizetype start, qsizetype end, const LayoutBox& box) {
    if (end <= start) return;
    const auto inset = LayoutBox::insets(box.usedBox);
    if (inset.left() > 0) edges.push_back({start, inset.left()});
    if (inset.right() > 0) edges.push_back({end, inset.right()});
  };
  for (const auto& span : projection_.spans()) {
    if (span.type != InlineType::Code || span.kind != InlineSpanKind::Text) continue;
    const auto range = layoutDisplayRangeForProjectionRange(span.displayStart, span.displayEnd);
    if (range.valid) addBox(range.start, range.end, *spanStyles_.value(span.displayStart, codeStyle_));
  }
  for (const auto& run : htmlInlineBoxRuns_) addBox(run.start, run.end, run.box);
  if (edges.isEmpty()) return;
  std::sort(edges.begin(), edges.end(), [](const auto& a, const auto& b) { return a.position < b.position; });
  QVector<Edge> unique;
  for (const auto& edge : edges) {
    if (!unique.isEmpty() && unique.back().position == edge.position)
      unique.back().width += edge.width;
    else
      unique.push_back(edge);
  }
  edges = std::move(unique);
  QVector<qsizetype> positions;
  for (const auto& edge : edges) positions.push_back(edge.position);
  const auto inserted = displayMap_.insertSpacing(std::move(positions), kInlineBoxSpacer);
  const auto before = [&](qsizetype p) { return inserted.before(p); };
  const auto after = [&](qsizetype p) { return inserted.after(p); };
  for (const auto& edge : edges) inlineSpacers_.push_back({before(edge.position), edge.width});
  for (auto& atom : mathAtoms_) {
    atom.displayEnd = before(atom.displayEnd);
    atom.displayStart = after(atom.displayStart);
  }
  for (auto& atom : imageAtoms_) {
    atom.displayEnd = before(atom.displayEnd);
    atom.displayStart = after(atom.displayStart);
  }
  for (auto& atom : htmlAtoms_) {
    atom.displayEnd = before(atom.displayEnd);
    atom.displayStart = after(atom.displayStart);
  }
}

void InlineLayout::insertGeneratedPseudoText(const QString& text, const PseudoElementRule& rule,
                                             GeneratedPseudoAtom::Kind kind, const QFont& font,
                                             qreal width, qreal height, qreal marginLeft,
                                             qreal marginRight, bool before, qsizetype projectionAnchor,
                                             qsizetype visibleAnchor, QString href) {
  if (text.isEmpty()) return;
  QString inserted = text;
  const bool leftSpacer = kind == GeneratedPseudoAtom::Kind::Text && !qFuzzyIsNull(marginLeft);
  const bool rightSpacer = kind == GeneratedPseudoAtom::Kind::Text && !qFuzzyIsNull(marginRight);
  if (leftSpacer) inserted.prepend(kInlineBoxSpacer);
  if (rightSpacer) inserted.append(kInlineBoxSpacer);
  const bool block = rule.computed && rule.computed->resolvedValue("display") == "block";
  const bool atomicBreak = !block && kind != GeneratedPseudoAtom::Kind::Text;
  // TextLayout reserves U+FFFC for QTextDocument's object handlers. Keep the
  // calibrated glyph and explicitly supply the atomic box's UAX #14 break
  // opportunity at its adjoining text edge instead of joining it to a word.
  if (atomicBreak) {
    inserted.append(QChar(0x200b));
    inserted.prepend(QChar(0x200b));
  }
  if (block) {
    if (before) inserted.append(QChar::LineSeparator);
    else inserted.prepend(QChar::LineSeparator);
  }
  const qsizetype position = layoutDisplayOffsetForProjectionOffset(projectionAnchor,
      before ? InlineProjectionBias::Forward : InlineProjectionBias::Backward);
  const qsizetype length = inserted.size();

  const auto shift = [&](qsizetype& start, qsizetype& end) {
    if (start >= position) {
      start += length;
      end += length;
    } else if (end > position) {
      end += length;
    }
  };
  for (auto& atom : mathAtoms_) shift(atom.displayStart, atom.displayEnd);
  for (auto& atom : imageAtoms_) shift(atom.displayStart, atom.displayEnd);
  for (auto& atom : htmlAtoms_) shift(atom.displayStart, atom.displayEnd);
  for (auto& atom : generatedPseudoAtoms_) {
    shift(atom.displayStart, atom.displayEnd);
    shift(atom.rangeStart, atom.rangeEnd);
  }
  for (auto& spacer : inlineSpacers_) {
    if (spacer.start >= position) spacer.start += length;
  }
  for (auto& format : generatedPseudoFormats_) {
    if (format.start >= position) format.start += static_cast<int>(length);
  }

  displayMap_.insertGenerated(position, inserted, projectionAnchor, visibleAnchor);

  GeneratedPseudoAtom atom;
  atom.rangeStart = position;
  atom.rangeEnd = position + length;
  atom.displayStart = position + (leftSpacer ? 1 : 0) + ((block && !before) || atomicBreak ? 1 : 0);
  atom.displayEnd = atom.displayStart + text.size();
  atom.advance = kind != GeneratedPseudoAtom::Kind::Text ? width + marginLeft + marginRight : 0.0;
  atom.width = width;
  atom.height = height;
  atom.marginLeft = marginLeft;
  atom.marginRight = marginRight;
  atom.baseline = height;
  atom.kind = kind;
  atom.text = kind == GeneratedPseudoAtom::Kind::Text ? text : QString();
  atom.rule = rule;
  atom.font = font;
  atom.block = block;
  atom.before = before;
  atom.visibleAnchor = visibleAnchor;
  atom.href = std::move(href);

  QTextCharFormat format;
  if (kind == GeneratedPseudoAtom::Kind::Text) {
    format.setFont(font);
    if (rule.color.isValid()) {
      auto color = rule.color;
      color.setAlphaF(color.alphaF() * rule.opacity);
      format.setForeground(color);
    }
  } else {
    QFont markerFont = font;
    markerFont.setLetterSpacing(QFont::AbsoluteSpacing, 0);
    const qreal glyphWidthWithoutSpacing = TextFontMetrics(markerFont).horizontalAdvance(kGeneratedPseudoPlaceholder);
    markerFont.setLetterSpacing(QFont::AbsoluteSpacing, atom.advance - glyphWidthWithoutSpacing);
    format.setFont(markerFont);
    format.setForeground(Qt::transparent);
  }
  generatedPseudoFormats_.push_back({static_cast<int>(atom.displayStart), static_cast<int>(text.size()), format});
  if (leftSpacer) inlineSpacers_.push_back({atom.displayStart - 1, marginLeft});
  if (rightSpacer) inlineSpacers_.push_back({atom.displayEnd, marginRight});
  generatedPseudoAtoms_.push_back(std::move(atom));
}

void InlineLayout::buildGeneratedPseudoAtoms(const RenderTheme& theme, const QFont& baseFont, qreal width,
                                             const BuildOptions& options) {
  pseudoBeforeFingerprint_ = options.pseudoBefore && options.pseudoBefore->computed ? options.pseudoBefore->computed->fingerprint() : 0;
  pseudoAfterFingerprint_ = options.pseudoAfter && options.pseudoAfter->computed ? options.pseudoAfter->computed->fingerprint() : 0;
  const auto add = [&](const PseudoElementRule* rule, const QString& resolvedText, bool before,
                       qsizetype projectionAnchor, qsizetype visibleAnchor, const QString& href = QString()) {
    if (!rule || rule->absolute) return;
    const auto value = generatedContentStyle(theme, *rule, baseFont, {width, -1}, resolvedText);
    if (!value) return;
    const auto& v = *value;
    const auto inset = LayoutBox::insets(v.used);
    if (v.atomic) {
      insertGeneratedPseudoText(QString(kGeneratedPseudoPlaceholder), *rule,
          v.icon ? GeneratedPseudoAtom::Kind::Icon : GeneratedPseudoAtom::Kind::Shape,
          v.font, v.width, v.height, v.used.margin.left(), v.used.margin.right(), before,
          projectionAnchor, visibleAnchor, href);
    } else {
      insertGeneratedPseudoText(v.text, *rule, GeneratedPseudoAtom::Kind::Text, v.font, 0, 0,
          v.used.margin.left() + inset.left(), v.used.margin.right() + inset.right(), before,
          projectionAnchor, visibleAnchor, href);
    }
    auto& atom = generatedPseudoAtoms_.last();
    atom.box = LayoutBox::place(v.style.key, v.style, v.used, {}, v.font);
    if (v.icon && rule->svgFromMask) atom.box.style.paint.backgroundColor = QColor();
  };

  struct LinkRun {
    const CssElement* origin;
    qsizetype start, end, source;
    QString href;
  };
  QVector<LinkRun> links;
  const auto collect = [&](qsizetype start, qsizetype end, qsizetype source, const QString& href,
                           const CssElement* knownOrigin = nullptr) {
    if (end <= start || href.isEmpty()) return;
    const auto* origin = knownOrigin ? knownOrigin : options.styleNode ? theme.cssInlineElement(*options.styleNode, source, QStringLiteral("a")) : nullptr;
    for (auto& link : links) {
      if (origin ? link.origin == origin : link.end == start && link.href == href) {
        link.start = qMin(link.start, start); link.end = qMax(link.end, end); return;
      }
    }
    links.push_back({origin, start, end, source, href});
  };
  for (const auto& span : projection_.spans()) {
    if (!span.link || span.kind == InlineSpanKind::OpenMarker || span.kind == InlineSpanKind::CloseMarker ||
        span.kind == InlineSpanKind::HiddenSyntax || span.kind == InlineSpanKind::EmptyContentSlot) continue;
    collect(span.displayStart, span.displayEnd, span.contentSourceStart + styleSourceBase_,
            projection_.linkHrefAtDisplayOffset(span.displayStart));
  }
  for (const auto& html : projection_.htmlFormatData()) {
    if (!html.atomicHtml.isEmpty()) continue;
    for (const auto& link : html.links) {
      qsizetype source = 0;
      const auto start = html.displayStart + link.start;
      if (projection_.sourceOffsetForDisplayOffset(start, InlineProjectionBias::Forward, source)) {
        const CssElement* origin = nullptr;
        if (options.styleNode) for (const auto& span : projection_.spans())
          if (span.displayStart <= start && start < span.displayEnd)
            origin = theme.cssLinkInSourceRange(*options.styleNode, span.sourceStart + styleSourceBase_,
                span.sourceEnd + styleSourceBase_, link.href);
        collect(start, start + link.length, source + styleSourceBase_, link.href, origin);
      }
    }
  }
  // Insert at stable projection boundaries, in reverse order. Source spans
  // remain untouched even when a link contains nested emphasis, math or HTML.
  std::sort(links.begin(), links.end(), [](const auto& a, const auto& b) { return a.start > b.start; });
  for (const auto& link : links) {
    for (const auto& pseudo : {QStringLiteral("after"), QStringLiteral("before")}) {
      std::optional<PseudoElementRule> rule;
      if (link.origin) rule = theme.pseudoForElement(*link.origin, pseudo);
      else {
        DocumentStylePrototypes prototypes;
        CssElement origin;
        origin.tag = QStringLiteral("a");
        origin.attributes.insert(QStringLiteral("href"), link.href);
        origin.parent = &prototypes.element(QStringLiteral("p"));
        rule = theme.pseudoForElement(origin, pseudo);
      }
      inlinePseudoFingerprints_.push_back({link.source, QStringLiteral("a"), pseudo,
          rule && rule->computed ? rule->computed->fingerprint() : quint64(0)});
      const bool before = pseudo == "before";
      const auto anchor = before ? link.start : link.end;
      const auto local = layoutDisplayOffsetForProjectionOffset(anchor,
          before ? InlineProjectionBias::Forward : InlineProjectionBias::Backward);
      add(rule ? &*rule : nullptr, {}, before, anchor, visibleOffsetForDisplayOffset(local), link.href);
    }
  }
  add(options.pseudoBefore ? &*options.pseudoBefore : nullptr, options.pseudoBeforeText, true, 0, 0);
  add(options.pseudoAfter ? &*options.pseudoAfter : nullptr, options.pseudoAfterText, false,
      projection_.displayText().size(), projection_.visibleText().size());
}

void InlineLayout::buildHtmlFormatSpans(const RenderTheme& theme, qreal width) {
  htmlFormatSpans_.clear();
  htmlInlineBoxRuns_.clear();
  const auto& data = projection_.htmlFormatData();
  for (const auto& hd : data) {
    if (!hd.atomicHtml.isEmpty()) continue;
    quintptr previousBoxId = 0;
    for (const auto& fs : hd.formatSpans) {
      // Map projection display offsets → layout display offsets
      const DisplayOffsetRange layoutRange =
          layoutDisplayRangeForProjectionRange(hd.displayStart + fs.start, hd.displayStart + fs.start + fs.length);
      if (!layoutRange.valid) {
        continue;
      }
      HtmlFormatSpan hs;
      hs.layoutStart = static_cast<int>(layoutRange.start);
      hs.layoutEnd = static_cast<int>(layoutRange.end);
      hs.bold = fs.bold;
      hs.italic = fs.italic;
      hs.monospace = fs.monospace;
      hs.decoration = fs.decoration;
      hs.color = fs.color;
      hs.backgroundColor = fs.backgroundColor;
      hs.fontSize = fs.fontSize;
      hs.lineHeight = fs.lineHeight;
      hs.verticalAlignment = fs.verticalAlignment;
      hs.keyboard = fs.keyboard;
      hs.fontFamilies = fs.fontFamilies;
      hs.font = fs.font;
      hs.fontSet = fs.fontSet;
      if (fs.inlineBoxId) {
        if (previousBoxId == fs.inlineBoxId && !htmlInlineBoxRuns_.isEmpty() && htmlInlineBoxRuns_.back().end == hs.layoutStart) {
          htmlInlineBoxRuns_.back().end = hs.layoutEnd;
        } else {
          const auto& style = fs.inlineBoxStyle;
          htmlInlineBoxRuns_.push_back(
              {hs.layoutStart, hs.layoutEnd, LayoutBox::place(style.key, style, theme.usedBoxForStyle(style, width), {}, fs.font)});
        }
      }
      previousBoxId = fs.inlineBoxId;
      htmlFormatSpans_.push_back(hs);
    }
  }
}

void InlineLayout::buildMathAtoms(const QVector<InlineNode>& inlines, const RenderTheme& theme, qreal width) {
  const QString projectedDisplay = projection_.displayText();
  QString rebuiltDisplay;
  QVector<OffsetMapEntry> rebuiltMap;
  QVector<DisplayOffsetMapEntry> rebuiltDisplayMap;
  for (const InlineProjectionSpan& span : projection_.spans()) {
    if (span.displayEnd <= span.displayStart) {
      continue;
    }

    const bool mathText = span.type == InlineType::InlineMath && span.kind == InlineSpanKind::Text;
    bool hasVisibleMarker = false;
    if (mathText) {
      for (const InlineProjectionSpan& marker : projection_.spans()) {
        if (marker.type == InlineType::InlineMath &&
            (marker.kind == InlineSpanKind::OpenMarker || marker.kind == InlineSpanKind::CloseMarker) &&
            marker.sourceStart >= span.sourceStart && marker.sourceEnd <= span.sourceEnd) {
          hasVisibleMarker = true;
          break;
        }
      }
    }
    const bool collapsed = mathText && !hasVisibleMarker;
    const QString spanText = projectedDisplay.mid(span.displayStart, span.displayEnd - span.displayStart);
    if (!collapsed) {
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});
      continue;
    }

    const QString tex = texForInlineMathSpan(inlines, span);
    if (tex.isEmpty()) {
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});
      continue;
    }
    auto layout = std::make_shared<math::MathLayoutResult>(mathRenderer_.render(tex, theme, false, qMax<qreal>(1.0, width)));
    if (!layout->valid()) {
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});
      continue;
    }

    const qsizetype displayStart = rebuiltDisplay.size();
    rebuiltDisplay += kInlineMathPlaceholder;
    MathAtom atom;
    atom.displayStart = displayStart;
    atom.displayEnd = rebuiltDisplay.size();
    atom.sourceStart = span.sourceStart;
    atom.sourceEnd = span.sourceEnd;
    atom.contentSourceStart = span.contentSourceStart;
    atom.contentSourceEnd = span.contentSourceEnd;
    atom.visibleStart = span.visibleStart;
    atom.visibleEnd = span.visibleEnd;
    atom.layout = std::move(layout);
    rebuiltMap.push_back(OffsetMapEntry{atom.displayStart, atom.displayEnd, atom.visibleStart, atom.visibleEnd});
    rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, atom.displayStart, atom.displayEnd});
    mathAtoms_.push_back(std::move(atom));
  }
  if (!rebuiltDisplay.isEmpty()) {
    displayMap_.assign(std::move(rebuiltDisplay), std::move(rebuiltMap), std::move(rebuiltDisplayMap));
  }
}

QString InlineLayout::texForInlineMathSpan(const QVector<InlineNode>& inlines, const InlineProjectionSpan& span) const {
  const QString source = projection_.sourceText();
  if (span.contentSourceStart >= 0 && span.contentSourceEnd >= span.contentSourceStart && span.contentSourceEnd <= source.size()) {
    const QString tex = source.mid(span.contentSourceStart, span.contentSourceEnd - span.contentSourceStart);
    if (!tex.isEmpty()) {
      return tex;
    }
  }

  const QString expected = projection_.visibleText().mid(span.visibleStart, span.visibleEnd - span.visibleStart);
  const auto visit = [&](const auto& self, const QVector<InlineNode>& nodes) -> QString {
    for (const InlineNode& node : nodes) {
      if (node.type() == InlineType::InlineMath && (expected.isEmpty() || node.text() == expected)) {
        return node.text();
      }
      if (!node.children().isEmpty()) {
        const QString found = self(self, node.children());
        if (!found.isEmpty()) {
          return found;
        }
      }
    }
    return QString();
  };
  return visit(visit, inlines);
}

void InlineLayout::buildImageAtoms(const QVector<InlineNode>& inlines, const RenderTheme& theme, qreal width, const QString& documentPath) {
  Q_UNUSED(inlines);
  // Quick check: are there any image Atom spans at all?
  bool hasImageAtom = false;
  for (const InlineProjectionSpan& span : projection_.spans()) {
    if (span.type == InlineType::Image && span.kind == InlineSpanKind::Atom && span.displayEnd > span.displayStart) {
      hasImageAtom = true;
      break;
    }
  }
  if (!hasImageAtom) {
    return;
  }

  // Iterate projection spans directly (same pattern as buildMathAtoms).
  const QString projectedDisplay = projection_.displayText();
  QString rebuiltDisplay;
  QVector<OffsetMapEntry> rebuiltMap;
  QVector<DisplayOffsetMapEntry> rebuiltDisplayMap;

  for (const InlineProjectionSpan& span : projection_.spans()) {
    if (span.displayEnd <= span.displayStart) {
      continue;
    }

    const bool isImageAtom = span.type == InlineType::Image && span.kind == InlineSpanKind::Atom;
    if (!isImageAtom) {
      const auto previous = layoutDisplayRangeForProjectionRange(span.displayStart, span.displayEnd);
      const QString spanText = previous.valid ? displayMap_.text().mid(previous.start, previous.end - previous.start)
                                              : projectedDisplay.mid(span.displayStart, span.displayEnd - span.displayStart);
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      for (auto& atom : mathAtoms_)
        if (previous.valid && atom.displayStart == previous.start) {
          atom.displayStart = displayStart;
          atom.displayEnd = rebuiltDisplay.size();
        }
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});
      continue;
    }

    const bool collapsed = !isImageSourceRevealed(projection_.spans(), span);

    const QString srcUrl = span.href;
    if (srcUrl.isEmpty()) {
      const QString spanText = projectedDisplay.mid(span.displayStart, span.displayEnd - span.displayStart);
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});
      continue;
    }

    const QUrl resolved = resolvedUrlForDocumentResource(srcUrl, documentPath);
    const QString resourceKey = resolved.toString(QUrl::FullyEncoded);
    const bool isRemote = resolved.scheme() == QLatin1String("http") || resolved.scheme() == QLatin1String("https");
    QImage image = ImageLoader::instance().image(resourceKey);

    if (image.isNull()) {
      // Image not yet available — show a placeholder icon inline.
      const bool isLoading = isRemote && ImageLoader::instance().isPending(resourceKey);
      constexpr qreal kPlaceholderSize = 24.0;
      QImage placeholder = isLoading
          ? image_placeholder::loading(QSizeF(kPlaceholderSize, kPlaceholderSize))
          : image_placeholder::broken(QSizeF(kPlaceholderSize, kPlaceholderSize));

      if (!placeholder.isNull() && collapsed) {
        const qsizetype displayStart = rebuiltDisplay.size();
        rebuiltDisplay += kImagePlaceholder;

        ImageAtom atom;
        atom.displayStart = displayStart;
        atom.displayEnd = rebuiltDisplay.size();
        atom.sourceStart = span.sourceStart;
        atom.sourceEnd = span.sourceEnd;
        atom.visibleStart = span.visibleStart;
        atom.visibleEnd = span.visibleEnd;
        atom.srcUrl = srcUrl;
        atom.resourceUrl = resourceKey;
        atom.displaySize = QSizeF(kPlaceholderSize, kPlaceholderSize);
        atom.image = placeholder;
        atom.loaded = true;

        rebuiltMap.push_back(OffsetMapEntry{atom.displayStart, atom.displayEnd, atom.visibleStart, atom.visibleEnd});
        rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, atom.displayStart, atom.displayEnd});
        imageAtoms_.push_back(std::move(atom));
        continue;
      }

      // Fallback: keep the alt text as-is (placeholder render failed or cursor is on the image)
      const QString spanText = projectedDisplay.mid(span.displayStart, span.displayEnd - span.displayStart);
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});
      continue;
    }

    // Apply the image's own zoom (style="zoom:N%") — read straight from the source
    // snippet the span covers. Markdown images have no zoom (factor 1.0).
    const QString imgSource = projection_.sourceText().mid(span.sourceStart, span.sourceEnd - span.sourceStart);
    const qreal zoom = image_syntax::zoomFactor(imgSource);

    const auto imageBox = [&] {
      const auto snapshot = spanStyles_.value(span.displayStart);
      CssFormattingItem item;
      item.style = snapshot ? snapshot->style : ThemeElementStyle{};
      item.style.box = theme.usedBoxForStyle(item.style, width);
      // usedBoxForStyle has resolved padding against this line's containing
      // width. Freeze it so image sizing uses the same values as its snapshot.
      item.style.box.paddingLengths = {};
      item.style.box.marginLengths = {};
      item.naturalSize = QSizeF(image.size()) * zoom * theme.zoomPercent() / 100.0;
      const auto size = cssReplacedSize(item, width);
      return std::make_shared<const LayoutBox>(LayoutBox::place("img", item.style, item.style.box, QRectF({}, size)));
    }();
    const auto imageDisplaySize = imageBox->borderBox.size();
    if (collapsed) {
      // Inactive: replace alt text with placeholder, render image inline.
      const QSizeF displaySize = imageDisplaySize;

      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += kImagePlaceholder;

      ImageAtom atom;
      atom.displayStart = displayStart;
      atom.displayEnd = rebuiltDisplay.size();
      atom.sourceStart = span.sourceStart;
      atom.sourceEnd = span.sourceEnd;
      atom.visibleStart = span.visibleStart;
      atom.visibleEnd = span.visibleEnd;
      atom.srcUrl = srcUrl;
      atom.resourceUrl = resourceKey;
      atom.displaySize = displaySize;
      atom.cssBox = imageBox;
      atom.image = image;
      atom.loaded = true;

      rebuiltMap.push_back(OffsetMapEntry{atom.displayStart, atom.displayEnd, atom.visibleStart, atom.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, atom.displayStart, atom.displayEnd});
      imageAtoms_.push_back(std::move(atom));
    } else {
      // Active (cursor on image): show source text as-is, add block preview below.
      const QString spanText = projectedDisplay.mid(span.displayStart, span.displayEnd - span.displayStart);
      const qsizetype displayStart = rebuiltDisplay.size();
      rebuiltDisplay += spanText;
      rebuiltMap.push_back(OffsetMapEntry{displayStart, rebuiltDisplay.size(), span.visibleStart, span.visibleEnd});
      rebuiltDisplayMap.push_back(DisplayOffsetMapEntry{span.displayStart, span.displayEnd, displayStart, rebuiltDisplay.size()});

      ImageAtom preview;
      preview.srcUrl = srcUrl;
      preview.displaySize = imageDisplaySize;
      preview.cssBox = imageBox;
      preview.image = image;
      preview.loaded = true;
      previewAtoms_.push_back(std::move(preview));
    }
  }

  if (!rebuiltDisplay.isEmpty()) {
    displayMap_.assign(std::move(rebuiltDisplay), std::move(rebuiltMap), std::move(rebuiltDisplayMap));
  }
}

void InlineLayout::buildHtmlAtoms(qreal width, const QFont& font, const QString& documentPath) {
  html::HtmlRenderer renderer;
  for (auto it = projection_.htmlFormatData().crbegin(); it != projection_.htmlFormatData().crend(); ++it) {
    const auto& data = *it;
    if (data.atomicHtml.isEmpty()) continue;
    for (const auto& span : projection_.spans()) {
      if (span.kind != InlineSpanKind::HtmlContent || span.displayStart != data.displayStart) continue;
      const auto range = layoutDisplayRangeForProjectionRange(span.displayStart, span.displayEnd);
      if (!range.valid || range.end <= range.start) continue;
      auto layout = std::make_shared<html::HtmlLayoutResult>(
          renderer.render(data.atomicHtml, font.pointSizeF(), width, QFileInfo(documentPath).absolutePath(), data.palette));
      if (!layout->valid() || !layout->root() || layout->root()->children().empty()) continue;
      const auto& box = *layout->root()->children().front();
      const auto& geo = box.geometry();
      HtmlAtom atom;
      atom.displayStart = range.start;
      atom.displayEnd = range.start + 1;
      atom.sourceStart = span.sourceStart;
      atom.sourceEnd = span.sourceEnd;
      atom.visibleStart = span.visibleStart;
      atom.visibleEnd = span.visibleEnd;
      atom.crop = QRectF(geo.left, geo.top, geo.width, geo.height);
      atom.baseline = box.firstBaseline >= 0 ? box.firstBaseline : geo.height;
      atom.margin = box.style().margin;
      atom.layout = std::move(layout);
      // Match DOM text runs in source order, including hidden runs, before
      // projecting their source positions into the visible fragment.
      QString decoded;
      QVector<qsizetype> decodedSource;
      for (qsizetype p = 0; p < data.atomicHtml.size();) {
        if (data.atomicHtml.mid(p, 4) == "<!--") {
          const auto end = data.atomicHtml.indexOf("-->", p + 4);
          p = end < 0 ? data.atomicHtml.size() : end + 3;
        } else if (data.atomicHtml[p] == QLatin1Char('<')) {
          const auto start = p++;
          QChar quote;
          for (; p < data.atomicHtml.size(); ++p) {
            const auto c = data.atomicHtml[p];
            if (!quote.isNull()) {
              if (c == quote) quote = {};
            } else if (c == QLatin1Char('\'') || c == QLatin1Char('"'))
              quote = c;
            else if (c == QLatin1Char('>')) {
              ++p;
              break;
            }
          }
          if (isStandaloneBrTag(data.atomicHtml.mid(start, p - start))) {
            decoded += QChar::LineSeparator;
            decodedSource += atom.sourceStart + start;
          }
        } else {
          const auto start = p;
          QString text = data.atomicHtml.mid(p++, 1);
          bool literal = true;
          if (text == "&") {
            const auto end = data.atomicHtml.indexOf(';', p);
            if (end >= p && end - p < 32) {
              text = QTextDocumentFragment::fromHtml(data.atomicHtml.mid(start, end - start + 1)).toPlainText();
              p = end + 1;
              literal = text == data.atomicHtml.mid(start, p - start);
            }
          }
          if (text == "\r") {
            if (p < data.atomicHtml.size() && data.atomicHtml[p] == QLatin1Char('\n')) ++p;
            text = "\n";
            literal = false;
          }
          decoded += text;
          for (qsizetype n = 0; n < text.size(); ++n) decodedSource += atom.sourceStart + start + (literal ? n : 0);
        }
      }
      atom.sourceOffsets.fill(atom.sourceEnd, qMax<qsizetype>(1, span.visibleEnd - span.visibleStart) + 1);
      qsizetype search = 0;
      const auto mapText = [&](const auto& self, const html::HtmlBox& child, bool visible) -> void {
        visible = visible && child.style().visible && child.style().display != html::HtmlDisplay::None;
        const auto text = child.isTextRun()                     ? child.text()
                          : child.tag() == html::HtmlTag::Break ? QString(QChar::LineSeparator)
                                                                : QString();
        if (!text.isEmpty()) {
          const auto at = decoded.indexOf(text, search);
          if (at >= 0) {
            if (visible)
              for (qsizetype n = 0; n < text.size() && child.plainTextStart + n < atom.sourceOffsets.size() - 1; ++n)
                atom.sourceOffsets[child.plainTextStart + n] = decodedSource[at + n];
            search = at + text.size();
          }
        }
        for (const auto& nested : child.children()) self(self, *nested, visible);
      };
      mapText(mapText, *atom.layout->root(), true);
      const qsizetype delta = 1 - (range.end - range.start);
      displayMap_.collapse(range, QChar(0xfffc));
      const auto shift = [&](auto& atoms) {
        for (auto& other : atoms)
          if (other.displayStart >= range.end) {
            other.displayStart += delta;
            other.displayEnd += delta;
          }
      };
      shift(mathAtoms_);
      shift(imageAtoms_);
      shift(htmlAtoms_);
      htmlAtoms_.push_back(std::move(atom));
    }
  }
}

QString InlineLayout::shapingText() const {
  QString shapingText = layoutTextForDisplayText(displayMap_.text());
  // Tab-indent projection markers use U+200B too, but generated object breaks
  // must remain zero-width breaks rather than becoming indentation NBSPs.
  for (const auto& atom : generatedPseudoAtoms_)
    if (!atom.block && atom.kind != GeneratedPseudoAtom::Kind::Text) {
      shapingText[atom.displayEnd] = QChar(0x200b);
      shapingText[atom.displayStart - 1] = QChar(0x200b);
    }
  return shapingText;
}

void InlineLayout::buildTextLayout(const RenderTheme& theme, qreal width, const QFont& baseFont) {
  QString shapingText = this->shapingText();
  QVector<QTextLayout::FormatRange> formats = textLayoutFormats(theme, baseFont, shapingText);

  int insertAt = -1;
  if (!preeditText_.isEmpty() && preeditInsertAtSourceOffset_ >= 0) {
    qsizetype displayOffset = -1;
    if (layoutDisplayOffsetForSourceOffset(preeditInsertAtSourceOffset_, InlineProjectionBias::Forward, displayOffset))
      insertAt = qBound(0, static_cast<int>(displayOffset), static_cast<int>(shapingText.size()));
  }
  composition_ = composeInlineText(std::move(shapingText), std::move(formats), insertAt,
                                   preeditText_, preeditFormats_,
                                   baseTextColorOverride_.isValid() ? baseTextColorOverride_ : theme.textColor());
  const auto& composedFormats = composition_.formats;

  textLayout_ = std::make_unique<TextLayout>(composition_.text.isEmpty() ? QStringLiteral(" ") : composition_.text, baseFont);
  // Retain the shaping snapshot together with the line geometry. Qt otherwise
  // frees it at endLayout() and reshapes during paint/caret queries, which can
  // resolve a different font-engine state after an incremental pass.
  textLayout_->setCacheEnabled(true);
  QTextOption option;
  option.setWrapMode(wrapMode_);
  if (alignment_ != Qt::Alignment()) {
    option.setAlignment(alignment_);
  }
  textLayout_->setTextOption(option);
  textLayout_->setFormats(composedFormats);
  // Pre-compute the runs a `:hover { color }` should recolour (only when the
  // element actually declares a hover colour). Derived from the same `formats`
  // the layout draws, so it stays in lock-step with span colouring.
  if (hoverTextColor_.isValid() || focusTextColor_.isValid()) {
    computeHoverRecolourRanges(composedFormats, theme);
  } else {
    hoverRecolourRanges_.clear();
  }

  struct LineRun { int start, end; QFont font; qreal lineHeight; bool shifted; };
  QVector<LineRun> lineRuns;
  for (const auto& span : projection_.spans()) {
    if (span.kind == InlineSpanKind::Atom || span.kind == InlineSpanKind::HtmlContent) continue;
    const auto range = layoutDisplayRangeForProjectionRange(span.displayStart, span.displayEnd);
    if (!range.valid || range.end <= range.start) continue;
    QFont font = span.type == InlineType::Code ? codeStyle_->font : baseFont;
    if (span.bold) font.setWeight(QFont::Bold);
    if (span.italic) font.setItalic(true);
    qreal multiplier = lineHeightMultiplier_;
    if (const auto snapshot = spanStyles_.value(span.displayStart); snapshot && span.kind == InlineSpanKind::Text) {
      font = snapshot->font;
      multiplier = snapshot->style.text.lineHeight;
    }
    lineRuns.push_back({toLayoutOffset(range.start), toLayoutOffset(range.end), font,
                       multiplier > 0 ? cssLineHeightPx(font.pointSizeF(), multiplier) : 0,
                       span.subscript || span.superscript});
  }
  for (const auto& span : htmlFormatSpans_) {
    QFont font = span.fontSet ? span.font : baseFont;
    if (span.fontSize > 0) font.setPointSizeF(span.fontSize);
    lineRuns.push_back({toLayoutOffset(span.layoutStart), toLayoutOffset(span.layoutEnd), font, span.lineHeight,
                       span.verticalAlignment != QTextCharFormat::AlignNormal});
  }
  for (const auto& atom : generatedPseudoAtoms_) {
    if (atom.kind == GeneratedPseudoAtom::Kind::Text) {
      lineRuns.push_back({toLayoutOffset(static_cast<int>(atom.displayStart)),
                          toLayoutOffset(static_cast<int>(atom.displayEnd)), atom.font,
                          cssLineHeightPx(atom.font.pointSizeF(), atom.box.style.text.lineHeight), false});
    }
  }
  std::sort(lineRuns.begin(), lineRuns.end(), [](const auto& a, const auto& b) { return a.start < b.start; });
  qsizetype firstRun = 0;
  const qreal lineWidth = qMax<qreal>(1.0, width);
  qreal height = 0.0;
  qreal maxWidth = 0.0;
  textLayout_->beginLayout();
  while (true) {
    TextLine line = textLayout_->createLine();
    if (!line.isValid()) {
      break;
    }
    line.setLineWidth(lineWidth);
    InlineLineBox lineBox(baseFont, lineHeightMultiplier_ > 0 ? cssLineHeightPx(baseFont.pointSizeF(), lineHeightMultiplier_) : 0);
    const int lineStart = line.textStart();
    const int lineEnd = lineStart + line.textLength();
    while (firstRun < lineRuns.size() && lineRuns[firstRun].end <= lineStart) ++firstRun;
    for (qsizetype i = firstRun; i < lineRuns.size() && lineRuns[i].start < lineEnd; ++i) {
      if (lineRuns[i].end <= lineStart) continue;
      lineBox.includeText(lineRuns[i].font, lineRuns[i].lineHeight);
      if (lineRuns[i].shifted) lineBox.includeAtomic(line.ascent(), line.height());
    }
    for (const auto& atom : htmlAtoms_) {
      const auto start = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (start >= lineStart && start < lineEnd) lineBox.includeAtomic(atom.baseline, atom.crop.height(), atom.margin);
    }
    for (const ImageAtom& atom : imageAtoms_) {
      const int start = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atom.loaded && start >= lineStart && start < lineEnd)
        lineBox.includeAtomic(atom.displaySize.height(), atom.displaySize.height());
    }
    for (const MathAtom& atom : mathAtoms_) {
      const int start = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atom.layout && atom.layout->valid() && start >= lineStart && start < lineEnd) {
        const auto used = mathUsedBounds(*atom.layout);
        lineBox.includeAtomic(-used.top(), used.height());
      }
    }
    const GeneratedPseudoAtom* blockAtom = nullptr;
    qreal topAlignedHeight = 0, bottomAlignedHeight = 0;
    for (const GeneratedPseudoAtom& atom : generatedPseudoAtoms_) {
      const auto start = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atom.kind != GeneratedPseudoAtom::Kind::Text && start >= lineStart && start < lineEnd) {
        if (atom.block) { blockAtom = &atom; continue; }
        const auto align = atom.rule.computed ? atom.rule.computed->resolvedValue("vertical-align") : QString();
        const auto margin = atom.box.usedBox.margin;
        if (align == "top") topAlignedHeight = qMax(topAlignedHeight, atom.height + margin.top() + margin.bottom());
        else if (align == "bottom") bottomAlignedHeight = qMax(bottomAlignedHeight, atom.height + margin.top() + margin.bottom());
        else if (align == "middle") lineBox.includeAtomic((atom.height + TextFontMetrics(baseFont).xHeight()) / 2, atom.height, margin);
        else lineBox.includeAtomic(atom.baseline, atom.height, margin);
      }
    }
    // Top/bottom atoms establish the minimum enclosing line height. Let the
    // tallest establish its edge first; otherwise the shorter atom can move
    // the text baseline unnecessarily before the tallest expands the line.
    if (bottomAlignedHeight > topAlignedHeight) {
      lineBox.includeBottomAligned(bottomAlignedHeight);
      lineBox.includeTopAligned(topAlignedHeight);
    } else {
      lineBox.includeTopAligned(topAlignedHeight);
      lineBox.includeBottomAligned(bottomAlignedHeight);
    }
    qreal allocatedHeight;
    if (blockAtom) {
      allocatedHeight = blockAtom->height + blockAtom->box.usedBox.margin.top() + blockAtom->box.usedBox.margin.bottom();
      line.setPosition({0, height});
    } else {
      allocatedHeight = lineBox.placeLine(line, height);
    }
    lineBoxes_.push_back(QRectF(0, height, lineWidth, allocatedHeight));
    for (auto& atom : htmlAtoms_) {
      const auto start = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (start >= lineStart && start < lineEnd)
        atom.rect = QRectF(QPointF(line.cursorToX(start) + atom.margin.left(), line.y() + line.ascent() - atom.baseline), atom.crop.size());
    }
    for (auto& atom : generatedPseudoAtoms_) {
      const auto start = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atom.kind != GeneratedPseudoAtom::Kind::Text && start >= lineStart && start < lineEnd) {
        const bool topAligned = atom.rule.computed && atom.rule.computed->resolvedValue("vertical-align") == "top";
        const bool bottomAligned = atom.rule.computed && atom.rule.computed->resolvedValue("vertical-align") == "bottom";
        const bool middleAligned = atom.rule.computed && atom.rule.computed->resolvedValue("vertical-align") == "middle";
        atom.baseline = middleAligned ? (atom.height + TextFontMetrics(baseFont).xHeight()) / 2 : atom.height;
        atom.rect = QRectF(line.cursorToX(start) + atom.marginLeft,
                           topAligned ? height + atom.box.usedBox.margin.top() : bottomAligned
                               ? height + allocatedHeight - atom.height - atom.box.usedBox.margin.bottom()
                               : line.y() + line.ascent() - atom.baseline,
                           atom.width, atom.height);
        if (atom.block) {
          qreal x = atom.marginLeft;
          const qreal spare = qMax<qreal>(0, lineWidth - atom.width - atom.marginLeft - atom.marginRight);
          if (atom.box.usedBox.marginLeftAuto) x += spare / (atom.box.usedBox.marginRightAuto ? 2 : 1);
          atom.rect.moveTopLeft({x, height + atom.box.usedBox.margin.top()});
        }
        atom.box = LayoutBox::place(atom.box.hostKey, atom.box.style, atom.box.usedBox, atom.rect, atom.font);
        GeneratedContentStyle generated; generated.rule = atom.rule;
        atom.iconRect = generatedIconRect(generated, atom.box.contentBox, zoomScale_);
      }
    }
    height += allocatedHeight;
    maxWidth = qMax(maxWidth, line.naturalTextWidth());
  }
  textLayout_->endLayout();

  // Reserve space for block-level image previews (shown when cursor is on an image).
  constexpr qreal kPreviewSpacing = 6.0;
  previewHeight_ = 0.0;
  for (const ImageAtom& atom : previewAtoms_) {
    previewHeight_ += atom.displaySize.height() + kPreviewSpacing;
  }
  if (!previewAtoms_.isEmpty()) {
    previewHeight_ += kPreviewSpacing;  // leading gap: paintImagePreview starts one spacing below the text
  }
  size_ = QSizeF(qMin(lineWidth, qMax<qreal>(maxWidth, 1.0)), height + previewHeight_);
}

void InlineLayout::computeHoverRecolourRanges(const QVector<QTextLayout::FormatRange>& formats, const RenderTheme& theme) {
  hoverRecolourRanges_.clear();
  const int n = displayMap_.text().size();
  if (n <= 0) { return; }
  baseRunColor_ = baseTextColorOverride_.isValid() ? baseTextColorOverride_ : theme.textColor();
  // Effective foreground per character, derived from the SAME format ranges the
  // layout draws: every span that sets its own foreground (link / code / del /
  // kbd / explicit HTML colour) overrides the base, and atom placeholders render
  // transparent. A run therefore "inherits the element colour" exactly when its
  // effective colour equals the base — those are the runs `:hover { color }`
  // should recolour. (SpellCheck ranges set only an underline, no foreground, so
  // misspelled prose still inherits and recolours — correct.)
  QVector<QColor> effective(n, baseRunColor_);
  for (const QTextLayout::FormatRange& fr : formats) {
    const QColor fg = fr.format.foreground().color();
    if (!fg.isValid()) { continue; }
    const int start = qBound(0, fr.start, n);
    const int end = qBound(0, fr.start + fr.length, n);
    for (int i = start; i < end; ++i) { effective[i] = fg; }
  }
  int i = 0;
  while (i < n) {
    if (effective[i] != baseRunColor_) { ++i; continue; }
    const int start = i;
    while (i < n && effective[i] == baseRunColor_) { ++i; }
    hoverRecolourRanges_.append({start, i});
  }
}

void InlineLayout::paintTextShadow(QPainter& painter, QPointF origin) const {
  if (!textLayout_ || size_.width() <= 0 || size_.height() <= 0) { return; }
  const qreal blur = qMax(qreal(0.0), textShadow_.blur);
  const qreal maxOff = qMax(qAbs(textShadow_.offset.x()), qAbs(textShadow_.offset.y()));
  const int pad = qCeil(blur + maxOff + 2.0);
  const QSize size(qCeil(size_.width()) + pad * 2, qCeil(size_.height()) + pad * 2);

  // Render the text once as an alpha mask (only its coverage shape matters).
  QImage mask(size, QImage::Format_ARGB32_Premultiplied);
  mask.fill(Qt::transparent);
  { QPainter mp(&mask); mp.setRenderHint(QPainter::TextAntialiasing, true); textLayout_->draw(&mp, QPointF(pad, pad)); }

  // Recolour the mask's alpha to the shadow colour: fill the colour, then keep it
  // only where the mask had coverage (DestinationIn).
  QImage shadow(size, QImage::Format_ARGB32_Premultiplied);
  shadow.fill(Qt::transparent);
  { QPainter sp(&shadow);
    sp.fillRect(shadow.rect(), textShadow_.color);
    sp.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    sp.drawImage(0, 0, mask);
  }
  if (blur > 0.0) { boxBlur(shadow, qMax(1, qRound(blur))); }
  painter.drawImage(QPointF(origin.x() - pad + textShadow_.offset.x(),
                            origin.y() - pad + textShadow_.offset.y()), shadow);
}

void InlineLayout::paintTextLayoutMathAtoms(QPainter& painter, QPointF origin) const {
  if (!textLayout_) {
    return;
  }
  for (const MathAtom& atom : mathAtoms_) {
    if (!atom.layout || !atom.layout->valid()) {
      continue;
    }
    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const TextLine line = textLayout_->lineAt(i);
      if (!line.isValid()) {
        continue;
      }
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      const int atomStart = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atomStart < lineStart || atomStart >= lineEnd) {
        continue;
      }
      const qreal x = line.cursorToX(atomStart);
      const qreal baseline = origin.y() + line.y() + line.ascent();
      atom.layout->paint(painter, QPointF(origin.x() + x, baseline - atom.layout->baseline));
      break;
    }
  }
}

void InlineLayout::refreshImageResources() {
  for (auto* atoms : {&imageAtoms_, &previewAtoms_})
    for (auto& atom : *atoms)
      if (!atom.resourceUrl.isEmpty()) {
        const auto current = ImageLoader::instance().cached(atom.resourceUrl);
        if (!current.isNull()) atom.image = current;
      }
}

void InlineLayout::paintTextLayoutImageAtoms(QPainter& painter, QPointF origin) const {
  if (!textLayout_) {
    return;
  }
  for (const ImageAtom& atom : imageAtoms_) {
    if (!atom.loaded || atom.image.isNull()) {
      continue;
    }
    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const TextLine line = textLayout_->lineAt(i);
      if (!line.isValid()) {
        continue;
      }
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      const int atomStart = toLayoutOffset(static_cast<int>(atom.displayStart));
      if (atomStart < lineStart || atomStart >= lineEnd) {
        continue;
      }
      const qreal x = line.cursorToX(atomStart);
      const qreal y = origin.y() + line.y() + line.ascent() - atom.displaySize.height();
      const QRectF targetRect(origin.x() + x, y, atom.displaySize.width(), atom.displaySize.height());
      if (atom.cssBox) paintLayoutBox(painter, *atom.cssBox, targetRect.topLeft());
      painter.drawImage(atom.cssBox ? atom.cssBox->contentBox.translated(targetRect.topLeft()) : targetRect, atom.image,
                        QRectF(atom.image.rect()));
      break;
    }
  }
}

void InlineLayout::paintGeneratedPseudoAtoms(QPainter& painter, QPointF origin) const {
  if (!textLayout_) return;
  for (const GeneratedPseudoAtom& atom : generatedPseudoAtoms_) {
    if (atom.kind == GeneratedPseudoAtom::Kind::Text) continue;
    const int start = toLayoutOffset(static_cast<int>(atom.displayStart));
    for (int i = 0; i < textLayout_->lineCount(); ++i) {
      const TextLine line = textLayout_->lineAt(i);
      if (!line.isValid()) continue;
      const int lineStart = line.textStart();
      const int lineEnd = lineStart + line.textLength();
      if (start < lineStart || start >= lineEnd) continue;
      paintLayoutBox(painter, atom.box, origin);
      if (atom.kind == GeneratedPseudoAtom::Kind::Icon) {
        const QColor tint = atom.rule.svgFromMask ? atom.rule.maskTint : atom.rule.color;
        painter.save();
        painter.setOpacity(painter.opacity() * atom.rule.opacity);
        painter.setClipRect(atom.box.contentBox.translated(origin), Qt::IntersectClip);
        DecorationPainter::paintIcon(painter, atom.rule.svgData, atom.iconRect.translated(origin), tint, atom.rule.svgFromMask);
        painter.restore();
      }
      break;
    }
  }
}

QVector<QRectF> InlineLayout::generatedPseudoRects() const {
  QVector<QRectF> result;
  for (const auto& atom : generatedPseudoAtoms_) {
    if (atom.kind == GeneratedPseudoAtom::Kind::Text)
      result += selectionRectsForDisplayOffsets(atom.displayStart, atom.displayEnd);
    else if (atom.box.borderBox.isValid()) result.push_back(atom.box.borderBox);
  }
  return result;
}

void InlineLayout::paintImagePreview(QPainter& painter, QPointF origin) const {
  if (previewAtoms_.isEmpty() || !textLayout_) {
    return;
  }
  // Paint preview images below the text layout, left-aligned.
  constexpr qreal kPreviewSpacing = 6.0;
  const qreal textHeight = size_.height() - previewHeight_;
  qreal y = origin.y() + textHeight + kPreviewSpacing;
  for (const ImageAtom& atom : previewAtoms_) {
    if (!atom.loaded || atom.image.isNull()) {
      continue;
    }
    const QRectF targetRect(origin.x(), y, atom.displaySize.width(), atom.displaySize.height());
    if (atom.cssBox) paintLayoutBox(painter, *atom.cssBox, targetRect.topLeft());
    painter.drawImage(atom.cssBox ? atom.cssBox->contentBox.translated(targetRect.topLeft()) : targetRect, atom.image,
                      QRectF(atom.image.rect()));
    y += atom.displaySize.height() + kPreviewSpacing;
  }
}

QVector<QTextLayout::FormatRange> InlineLayout::textLayoutFormats(
    const RenderTheme& theme, const QFont& baseFont, const QString& shapingText) const {
  InlineFormatInput input{projection_, displayMap_, shapingText, theme, baseFont, *codeStyle_,
                          spanStyles_, htmlFormatSpans_, inlineSpacers_, generatedPseudoFormats_,
                          isMisspelled_, baseTextColorOverride_, {}};
  for (const auto& atom : mathAtoms_) {
    if (atom.layout && atom.layout->valid() && atom.displayEnd > atom.displayStart)
      input.objects.push_back({atom.displayStart, atom.displayEnd - atom.displayStart,
                               atom.layout->size.width(), kInlineMathPlaceholder});
  }
  for (const auto& atom : imageAtoms_) {
    if (atom.loaded && atom.displayEnd > atom.displayStart)
      input.objects.push_back({atom.displayStart, atom.displayEnd - atom.displayStart,
                               atom.displaySize.width(), kImagePlaceholder});
  }
  for (const auto& atom : htmlAtoms_)
    input.objects.push_back({atom.displayStart, 1, atom.crop.width() + atom.margin.left() + atom.margin.right(),
                             QChar(0xfffc), false, true});
  return buildInlineTextFormats(input);
}

qsizetype InlineLayout::visibleOffsetForDisplayOffset(qsizetype displayOffset) const {
  if (displayMap_.empty()) {
    return qBound<qsizetype>(0, displayOffset, projection_.visibleText().size());
  }
  displayOffset = qBound<qsizetype>(0, displayOffset, displayMap_.text().size());
  for (const MathAtom& atom : mathAtoms_) {
    if (displayOffset > atom.displayStart && displayOffset <= atom.displayEnd) {
      return atom.visibleEnd;
    }
  }
  for (const ImageAtom& atom : imageAtoms_) {
    if (displayOffset > atom.displayStart && displayOffset <= atom.displayEnd) {
      return atom.visibleEnd;
    }
  }
  return displayMap_.visibleOffsetForDisplayOffset(displayOffset);
}

qsizetype InlineLayout::displayOffsetForVisibleOffset(qsizetype visibleOffset) const {
  if (displayMap_.empty()) {
    return qBound<qsizetype>(0, visibleOffset, projection_.visibleText().size());
  }
  visibleOffset = qBound<qsizetype>(0, visibleOffset, projection_.visibleText().size());
  for (const MathAtom& atom : mathAtoms_) {
    if (visibleOffset > atom.visibleStart && visibleOffset < atom.visibleEnd) {
      return atom.displayEnd;
    }
  }
  for (const ImageAtom& atom : imageAtoms_) {
    if (visibleOffset > atom.visibleStart && visibleOffset < atom.visibleEnd) {
      return atom.displayEnd;
    }
  }
  return displayMap_.displayOffsetForVisibleOffset(visibleOffset);
}

qsizetype InlineLayout::projectionDisplayOffsetForLayoutOffset(qsizetype layoutOffset, InlineProjectionBias bias) const {
  return displayMap_.projectionDisplayOffsetForLayoutOffset(layoutOffset, bias);
}

qsizetype InlineLayout::layoutDisplayOffsetForProjectionOffset(qsizetype projectionOffset, InlineProjectionBias bias) const {
  return displayMap_.layoutDisplayOffsetForProjectionOffset(projectionOffset, bias);
}

bool InlineLayout::layoutDisplayOffsetForSourceOffset(qsizetype sourceOffset, InlineProjectionBias bias, qsizetype& layoutOffset) const {
  qsizetype projectionOffset = -1;
  if (!projection_.displayOffsetForSourceOffset(sourceOffset, bias, projectionOffset)) {
    return false;
  }
  layoutOffset = layoutDisplayOffsetForProjectionOffset(projectionOffset, bias);
  return true;
}

InlineLayout::DisplayOffsetRange InlineLayout::layoutDisplayRangeForProjectionRange(qsizetype projectionStart, qsizetype projectionEnd) const {
  return displayMap_.layoutDisplayRangeForProjectionRange(projectionStart, projectionEnd);
}

qsizetype InlineLayout::textLayoutDisplayOffsetForPoint(QPointF localPos) const {
  return textLayoutHitForPoint(localPos).displayOffset;
}

InlineLayout::TextLayoutPointHit InlineLayout::textLayoutHitForPoint(QPointF localPos) const {
  TextLayoutPointHit hit;
  if (!textLayout_) {
    return hit;
  }

  TextLine targetLine;
  for (int i = 0; i < textLayout_->lineCount(); ++i) {
    const TextLine line = textLayout_->lineAt(i);
    if (!line.isValid()) {
      continue;
    }
    const auto bounds = lineBoxes_[i];
    if (localPos.y() < bounds.top()) {
      break;
    }
    targetLine = line;
    if (localPos.y() < bounds.bottom()) {
      break;
    }
  }

  if (!targetLine.isValid() && textLayout_->lineCount() > 0) {
    targetLine = textLayout_->lineAt(0);
  }

  if (targetLine.isValid()) {
    const TextLine line = targetLine;
    const int lineStart = line.textStart();
    const int lineEnd = lineStart + line.textLength();
    if (lineEnd <= lineStart) {
      hit.displayOffset = composition_.toDisplayOffset(lineStart);
      hit.bias = InlineProjectionBias::Backward;
      hit.cursorRect = QRectF(line.cursorToX(lineStart), line.y(), 1.0, line.height());
      return hit;
    }

    if (localPos.x() <= line.cursorToX(lineStart)) {
      hit.displayOffset = composition_.toDisplayOffset(lineStart);
      hit.cursorRect = QRectF(line.cursorToX(lineStart), line.y(), 1.0, line.height());
      return hit;
    }
    if (localPos.x() >= line.cursorToX(lineEnd)) {
      hit.displayOffset = composition_.toDisplayOffset(lineEnd);
      hit.bias = InlineProjectionBias::Forward;
      hit.cursorRect = QRectF(line.cursorToX(lineEnd), line.y(), 1.0, line.height());
      return hit;
    }

    for (int offset = lineStart; offset < lineEnd; ++offset) {
      const qreal left = line.cursorToX(offset);
      const qreal right = line.cursorToX(offset + 1);
      const qreal midpoint = (left + right) / 2.0;
      if (localPos.x() < midpoint) {
        hit.displayOffset = composition_.toDisplayOffset(offset);
        hit.bias = InlineProjectionBias::Backward;
        hit.cursorRect = QRectF(line.cursorToX(offset), line.y(), 1.0, line.height());
        return hit;
      }
      if (localPos.x() < right) {
        hit.displayOffset = composition_.toDisplayOffset(offset + 1);
        hit.bias = InlineProjectionBias::Forward;
        hit.cursorRect = QRectF(line.cursorToX(offset + 1), line.y(), 1.0, line.height());
        return hit;
      }
    }
    hit.displayOffset = composition_.toDisplayOffset(lineEnd);
    hit.bias = InlineProjectionBias::Forward;
    hit.cursorRect = QRectF(line.cursorToX(lineEnd), line.y(), 1.0, line.height());
    return hit;
  }

  hit.displayOffset = displayMap_.text().size();
  hit.bias = InlineProjectionBias::Forward;
  hit.cursorRect = textLayoutCursorRectForDisplayOffset(hit.displayOffset);
  return hit;
}

QRectF InlineLayout::textLayoutCursorRectForDisplayOffset(qsizetype displayOffset) const {
  if (!textLayout_) {
    return {};
  }
  return textLayout_->cursorRect(toLayoutOffset(int(qBound<qsizetype>(0, displayOffset, displayMap_.text().size()))));
}

}  // namespace muffin
