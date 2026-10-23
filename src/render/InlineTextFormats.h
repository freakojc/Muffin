#pragma once

#include "render/InlineDisplayMap.h"
#include "render/LayoutBox.h"
#include "html/HtmlTextMeasurer.h"
#include "theme/RenderTheme.h"

#include <functional>
#include <memory>
#include <QTextLayout>

namespace muffin {

struct InlineHtmlFormatSpan {
  QStringList fontFamilies;
  QFont font;
  bool fontSet = false;
  int layoutStart = 0;
  int layoutEnd = 0;
  bool bold = false;
  bool italic = false;
  bool monospace = false;
  html::HtmlTextDecoration decoration = html::HtmlTextDecoration::None;
  QColor color;
  QColor backgroundColor;
  qreal fontSize = 0;
  qreal lineHeight = 0;
  QTextCharFormat::VerticalAlignment verticalAlignment = QTextCharFormat::AlignNormal;
  bool keyboard = false;
  QString href;
};

struct InlineSpacer {
  qsizetype start;
  qreal width;
};

// The measured advance is independent of the resource that supplied it.
struct InlineObjectFormat {
  qsizetype start = 0, length = 1;
  qreal width = 0;
  QChar placeholder;
  bool inheritBaseFormat = true;
  bool resetLetterSpacing = false;
};

// Read-only input in display coordinates, before IME composition. Format
// precedence is resolved here; resource creation and painting stay in the layout.
struct InlineFormatInput {
  const InlineProjection& projection;
  const InlineDisplayMap& displayMap;
  const QString& shapingText;
  const RenderTheme& theme;
  const QFont& font;
  const LayoutBox& codeStyle;
  const QHash<qsizetype, std::shared_ptr<const LayoutBox>>& spanStyles;
  const QVector<InlineHtmlFormatSpan>& htmlSpans;
  const QVector<InlineSpacer>& spacers;
  const QVector<QTextLayout::FormatRange>& generated;
  const std::function<bool(QStringView)>& isMisspelled;
  QColor foreground;
  QVector<InlineObjectFormat> objects;
};

QVector<QTextLayout::FormatRange> buildInlineTextFormats(const InlineFormatInput& input);

} // namespace muffin
