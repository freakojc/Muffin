#pragma once

#include "document/MarkdownNode.h"
#include "editor/CursorPosition.h"
#include "theme/RenderTheme.h"

namespace muffin::layout_rules {

// Shared flow rules consumed by both the cheap estimate and exact build paths.
bool isInsideBlockquote(const MarkdownNode& node);
// fast=true uses prototype CSS for the O(1) offscreen estimate path.
QMarginsF blockMarginForFlow(const MarkdownNode& node, const RenderTheme& theme, bool fast = false,
                             qreal containingWidth = -1);
bool hasCssFlowMargin(const MarkdownNode& node, const RenderTheme& theme, bool fast = false,
                      qreal containingWidth = -1);
qreal spacingBeforeInFlow(const MarkdownNode& node, const RenderTheme& theme, bool fast = false,
                          qreal containingWidth = -1);
qreal spacingAfterInFlow(const MarkdownNode& node, const RenderTheme& theme, bool fast = false,
                         qreal containingWidth = -1);
bool isTightListItemSiblingPair(const MarkdownNode& prev, const MarkdownNode& next);
bool isTightListNestedChildPair(const MarkdownNode& prev, const MarkdownNode& next);
qreal spacingBetweenInFlow(const MarkdownNode& prev, const MarkdownNode& next, const RenderTheme& theme,
                           bool fast = false, qreal containingWidth = -1);
bool isVirtualEmptyParagraphNode(const MarkdownNode& node);
bool omitVirtualEmptyParagraphInRenderFlow(const MarkdownNode& node, const SelectionRange& selection);
// Measured with the zoom-aware code font; includes one glyph of total gap.
qreal codeLineNumberGutterWidth(const QString& literal, const QFont& font);
QString displayLiteralFor(const MarkdownNode& node);
QVector<InlineNode> primaryInlinesForListItem(const MarkdownNode& node);

} // namespace muffin::layout_rules
