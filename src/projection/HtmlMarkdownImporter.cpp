#include "projection/HtmlMarkdownImporter.h"
#include "blocks/html/HtmlSanitizer.h"
#include "html/HtmlParser.h"
#include "html/HtmlBoxBuilder.h"
#include "theme/CssThemeParser.h"
#include <QRegularExpression>
#include <QUrl>

namespace {
using muffin::html::HtmlBox;
using muffin::html::HtmlTag;

QString escape(QString text) {
  QString result;
  for (const auto c : text) {
    if (QStringLiteral("\\`*_{}[]<>()!#|~+-.=&$").contains(c))
      result += '\\';
    result += c;
  }
  return result;
}

QString rawText(const HtmlBox& box) {
  if (box.tag() == HtmlTag::TextRun)
    return box.text();
  if (box.tag() == HtmlTag::Break)
    return QStringLiteral("\n");
  QString result;
  for (const auto& child : box.children())
    result += rawText(*child);
  return result;
}

bool isHeading(HtmlTag tag) {
  return tag >= HtmlTag::Heading1 && tag <= HtmlTag::Heading6;
}

bool isBlock(HtmlTag tag) {
  return isHeading(tag) || tag == HtmlTag::Paragraph || tag == HtmlTag::Div || tag == HtmlTag::Section ||
         tag == HtmlTag::Article || tag == HtmlTag::Pre || tag == HtmlTag::BlockQuote || tag == HtmlTag::Hr ||
         tag == HtmlTag::UnorderedList || tag == HtmlTag::OrderedList || tag == HtmlTag::ListItem ||
         tag == HtmlTag::Table || tag == HtmlTag::TableHead || tag == HtmlTag::TableBody || tag == HtmlTag::TableRow ||
         tag == HtmlTag::TableCell || tag == HtmlTag::TableHeader;
}

bool isCheckbox(const HtmlBox& box) {
  return box.tag() == HtmlTag::Input && box.cssAttributes.value("type").compare("checkbox", Qt::CaseInsensitive) == 0;
}

bool normalize(HtmlBox& box, int depth = 0, bool literal = false);

bool containsBlocks(const HtmlBox& box) {
  for (const auto& child : box.children())
    if (isBlock(child->tag()) || containsBlocks(*child))
      return true;
  return false;
}

struct InlineWrapper {
  HtmlTag tag;
  QString href;
  QString style;
};

// HTML permits <a><h3>Title</h3><p>Body</p></a>. Markdown needs the link
// inside each block. Apply inline wrappers to runs of inline children, keeping
// headings/list items/table cells as structural containers rather than labels.
void wrapInlineRuns(HtmlBox& box, const InlineWrapper& wrapper, int depth) {
  auto input = std::move(box.children());
  box.children().clear();
  std::unique_ptr<HtmlBox> run;
  const auto flush = [&] {
    if (run) {
      normalize(*run, depth + 1);
      box.addChild(std::move(run));
    }
  };
  for (auto& child : input) {
    if (isBlock(child->tag()) || containsBlocks(*child)) {
      flush();
      // A fenced code block cannot itself be a Markdown link/strong span.
      // Preserve its literal contents rather than projecting markup into it.
      if (child->tag() != HtmlTag::Pre)
        wrapInlineRuns(*child, wrapper, depth + 1);
      box.addChild(std::move(child));
    } else {
      if (!run) {
        run = std::make_unique<HtmlBox>(wrapper.tag);
        run->setHref(wrapper.href);
        run->cssInlineStyle = wrapper.style;
      }
      run->addChild(std::move(child));
    }
  }
  flush();
}

// Normalize the existing tree before adding any Markdown delimiters. A link,
// emphasis wrapper or heading has content only when a descendant does; syntax
// such as empty code ticks must never make an otherwise empty wrapper meaningful.
bool normalize(HtmlBox& box, int depth, bool literal) {
  QHash<QString, muffin::CssDeclaration> declarations;
  for (const auto& declaration : muffin::CssThemeParser::parseDeclarations(box.cssInlineStyle)) {
    const auto previous = declarations.constFind(declaration.property);
    if (previous == declarations.cend() || !previous->important || declaration.important)
      declarations.insert(declaration.property, declaration);
  }
  const auto value = [&](const QString& property) { return declarations.value(property).value.toLower(); };
  if (depth > 128 || box.cssAttributes.contains("hidden") || value("display") == "none") {
    box.setTag(HtmlTag::Span);
    box.children().clear();
    box.cssInlineStyle.clear();
    return false;
  }
  const auto tag = box.tag();
  if (tag == HtmlTag::TextRun) {
    if (literal)
      return !box.text().isEmpty();
    // Formatting characters alone (e.g. a zero-width permalink label) do not
    // constitute a link label. Preserve the original text for actual content.
    for (const auto c : box.text())
      if (!c.isSpace() && c.category() != QChar::Other_Format)
        return true;
    QString whitespace;
    for (const auto c : box.text())
      if (c.isSpace())
        whitespace += c;
    box.setText(std::move(whitespace));
    return false;
  }
  bool content = false;
  const bool literalChildren = literal || tag == HtmlTag::Code || tag == HtmlTag::Kbd || tag == HtmlTag::Pre;
  for (auto& child : box.children())
    content = normalize(*child, depth + 1, literalChildren) || content;
  if (tag == HtmlTag::Code || tag == HtmlTag::Kbd || tag == HtmlTag::Pre)
    content = !rawText(box).isEmpty(); // Spaces in literal code are content.
  if (tag == HtmlTag::Image)
    content = !box.src().isEmpty() || !box.alt().isEmpty();
  if (tag == HtmlTag::Hr || isCheckbox(box))
    content = true;
  if (!content && tag != HtmlTag::Break && tag != HtmlTag::TableCell && tag != HtmlTag::TableHeader &&
      tag != HtmlTag::TableRow && tag != HtmlTag::TableHead && tag != HtmlTag::TableBody) {
    // Inline whitespace still separates neighbouring words. Empty block
    // scaffolding carries no paragraph break; neither produces markup.
    if (isBlock(tag))
      box.children().clear();
    box.setTag(HtmlTag::Span);
    box.cssInlineStyle.clear();
    return false;
  }
  auto& style = box.style();
  bool numericWeight = false;
  const auto weight = value("font-weight").toInt(&numericWeight);
  style.fontWeight =
      !isHeading(tag) && (tag == HtmlTag::Strong || tag == HtmlTag::Bold || value("font-weight") == "bold" ||
                          (numericWeight && weight >= 600 && weight <= 1000))
          ? QFont::Bold
          : QFont::Normal;
  style.fontStyle = tag == HtmlTag::Em || tag == HtmlTag::Italic || value("font-style") == "italic"
                        ? QFont::StyleItalic
                        : QFont::StyleNormal;
  const bool inlineWrapper = tag == HtmlTag::Anchor || style.fontWeight == QFont::Bold ||
                             style.fontStyle == QFont::StyleItalic || tag == HtmlTag::Strikethrough ||
                             tag == HtmlTag::Del || tag == HtmlTag::Underline || tag == HtmlTag::Ins;
  if (!literalChildren && inlineWrapper && containsBlocks(box)) {
    const InlineWrapper wrapper{isBlock(tag) ? HtmlTag::Span : tag, box.href(), box.cssInlineStyle};
    if (!isBlock(tag))
      box.setTag(HtmlTag::Span);
    box.cssInlineStyle.clear();
    style.fontWeight = QFont::Normal;
    style.fontStyle = QFont::StyleNormal;
    wrapInlineRuns(box, wrapper, depth);
  }
  return content;
}

// Block boundaries are metadata, never leading/trailing newlines embedded in
// every container. Joining transparent wrappers therefore cannot multiply blank
// lines, and literal newlines inside a fenced block remain untouched.
struct MarkdownFragment {
  QString text;
  bool startsBlock = false;
  bool endsBlock = false;

  static MarkdownFragment block(QString text) { return {std::move(text), true, true}; }

  void append(MarkdownFragment next) {
    if (next.text.isEmpty())
      return;
    if (endsBlock && next.text.trimmed().isEmpty())
      return;
    if (text.isEmpty()) {
      *this = std::move(next);
      return;
    }
    if (endsBlock || next.startsBlock) {
      while (!text.isEmpty() && text.back().isSpace())
        text.chop(1);
      next.text = next.text.trimmed();
      if (text.isEmpty()) {
        *this = std::move(next);
        return;
      }
      text += QStringLiteral("\n\n");
    }
    text += next.text;
    endsBlock = next.endsBlock;
  }
};

QString fence(const QString& text, qsizetype minimum) {
  qsizetype longest = 0, run = 0;
  for (const auto c : text) {
    run = c == '`' ? run + 1 : 0;
    longest = qMax(longest, run);
  }
  return QString(qMax(minimum, longest + 1), '`');
}

QString destination(const QString& url) {
  return QString::fromUtf8(QUrl(url).toEncoded()).replace('(', "%28").replace(')', "%29").replace('|', "%7C");
}

QString wrap(QString content, const QString& prefix, const QString& suffix) {
  qsizetype start = 0, end = content.size();
  while (start < end && content[start].isSpace())
    ++start;
  while (end > start && content[end - 1].isSpace())
    --end;
  if (start == end)
    return content;
  return content.first(start) + prefix + content.mid(start, end - start) + suffix + content.mid(end);
}

MarkdownFragment convert(const HtmlBox& box, int depth);

MarkdownFragment children(const HtmlBox& box, int depth) {
  MarkdownFragment result;
  for (const auto& child : box.children())
    result.append(convert(*child, depth + 1));
  return result;
}

QString convertTable(const HtmlBox& table, int depth) {
  QList<QStringList> rows;
  const auto collect = [&](const auto& self, const HtmlBox& box) -> void {
    if (box.tag() == HtmlTag::TableRow) {
      QStringList row;
      for (const auto& cell : box.children()) {
        if (cell->tag() != HtmlTag::TableCell && cell->tag() != HtmlTag::TableHeader)
          continue;
        auto value = children(*cell, depth + 1).text.trimmed();
        value.replace(QStringLiteral("\n\n"), QStringLiteral("<br>"));
        value.replace('\n', QStringLiteral("<br>"));
        row.append(value);
        for (int i = 1; i < qBound(1, cell->colSpan(), 1000); ++i)
          row.append(QString());
      }
      if (!row.isEmpty())
        rows.append(row);
    } else if (box.tag() != HtmlTag::Table || &box == &table) {
      for (const auto& child : box.children())
        self(self, *child);
    }
  };
  collect(collect, table);
  qsizetype columns = 0;
  for (const auto& row : rows)
    columns = qMax(columns, row.size());
  QString result;
  for (qsizetype i = 0; i < rows.size(); ++i) {
    result += '|';
    for (qsizetype col = 0; col < columns; ++col)
      result += ' ' + rows[i].value(col) + QStringLiteral(" |");
    result += '\n';
    if (i == 0)
      result += '|' + QStringLiteral(" --- |").repeated(columns) + '\n';
  }
  return result.trimmed();
}

MarkdownFragment convert(const HtmlBox& box, int depth) {
  if (depth > 128)
    return {};
  const auto tag = box.tag();
  if (tag == HtmlTag::TextRun) {
    QString text = box.text();
    static const QRegularExpression whitespace(QStringLiteral("[ \\t\\r\\n\\f]+"));
    text.replace(whitespace, QStringLiteral(" "));
    return {escape(text)};
  }
  if (tag == HtmlTag::Pre) {
    const auto text = rawText(box);
    const auto marker = fence(text, 3);
    QString language;
    if (!box.children().empty())
      for (const auto& name : box.children().front()->cssClasses)
        if (name.startsWith("language-"))
          language = name.mid(9).section(QRegularExpression("[^A-Za-z0-9_+.-]"), 0, 0);
    return MarkdownFragment::block(marker + language + '\n' + text +
                                   (text.endsWith('\n') ? QString() : QStringLiteral("\n")) + marker);
  }
  if (tag == HtmlTag::Code || tag == HtmlTag::Kbd) {
    const auto text = rawText(box).replace('\n', ' ');
    const auto marker = fence(text, 1);
    const bool needsPadding = text.startsWith('`') || text.endsWith('`') ||
                              (text.startsWith(' ') && text.endsWith(' ') && !text.trimmed().isEmpty());
    const auto padding = needsPadding ? QStringLiteral(" ") : QString();
    return {marker + padding + text + padding + marker};
  }
  if (tag == HtmlTag::Break)
    return {QStringLiteral("  \n")};
  if (tag == HtmlTag::Hr)
    return MarkdownFragment::block(QStringLiteral("---"));
  if (tag == HtmlTag::Table)
    return MarkdownFragment::block(convertTable(box, depth));
  if (isCheckbox(box))
    return {box.cssAttributes.contains("checked") ? QStringLiteral("[x] ") : QStringLiteral("[ ] ")};
  if (tag == HtmlTag::Image)
    return {box.src().isEmpty()
                ? escape(box.alt())
                : QStringLiteral("![") + escape(box.alt()) + QStringLiteral("](") + destination(box.src()) + ')'};
  if (tag == HtmlTag::UnorderedList || tag == HtmlTag::OrderedList) {
    QString result;
    int number = box.listStart();
    for (const auto& item : box.children()) {
      if (item->tag() != HtmlTag::ListItem)
        continue;
      const auto marker =
          tag == HtmlTag::OrderedList ? QString::number(number++) + QStringLiteral(". ") : QStringLiteral("- ");
      auto text = children(*item, depth + 1).text.trimmed();
      text.replace('\n', '\n' + QString(marker.size(), ' '));
      result += marker + text + '\n';
    }
    return MarkdownFragment::block(result.trimmed());
  }
  auto result = children(box, depth);
  QString& text = result.text;
  if (tag == HtmlTag::Anchor && !box.href().isEmpty())
    text = wrap(text, "[", QStringLiteral("](") + destination(box.href()) + ')');
  if (box.style().fontWeight == QFont::Bold)
    text = wrap(text, "**", "**");
  if (box.style().fontStyle == QFont::StyleItalic)
    text = wrap(text, "*", "*");
  if (tag == HtmlTag::Strikethrough || tag == HtmlTag::Del)
    text = wrap(text, "~~", "~~");
  if (tag == HtmlTag::Underline || tag == HtmlTag::Ins)
    text = wrap(text, "<u>", "</u>");
  if (tag == HtmlTag::BlockQuote) {
    text = text.trimmed();
    text.replace('\n', QStringLiteral("\n> "));
    return MarkdownFragment::block(QStringLiteral("> ") + text);
  }
  if (isHeading(tag))
    return MarkdownFragment::block(QString(int(tag) - int(HtmlTag::Heading1) + 1, '#') + ' ' + text.trimmed());
  if (tag == HtmlTag::Paragraph || tag == HtmlTag::Div || tag == HtmlTag::Section || tag == HtmlTag::Article)
    return MarkdownFragment::block(text.trimmed());
  return result;
}
}

QString muffin::importHtmlMarkdown(const QString& html) {
  html::HtmlDocument document;
  if (!document.parse(HtmlSanitizer().sanitizedClipboard(html)))
    return {};
  html::HtmlBoxBuilder builder;
  builder.setPreserveWhitespace(true);
  const auto root = builder.build(document);
  if (!root || !normalize(*root))
    return {};
  return convert(*root, 0).text.trimmed();
}
