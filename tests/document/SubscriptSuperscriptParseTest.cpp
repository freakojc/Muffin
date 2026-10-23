#include "document/InlineNode.h"
#include "document/MarkdownNode.h"
#include "parser/CmarkGfmParser.h"
#include "parser/MarkdownSerializer.h"

#include "../parser/ParserTestUtils.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QIODevice>
#include <QStringList>
#include <cstdio>

using namespace muffin;

// Subscript `H~2~O` and superscript `X^2^` (Pandoc-style). cmark-gfm has no extension for these, so
// the markers arrive as literal Text and a post-parse pass (gated on enableSubscript/enableSuperscript)
// splits them into typed inlines. Faithful to Pandoc: only a run of exactly one `~`/`^` is a marker,
// so `~~` (strikethrough) is never consumed as subscript. A backslash-escaped marker stays literal.

namespace {
int countInline(const QVector<InlineNode>& inlines, InlineType type) {
  int count = 0;
  for (const InlineNode& node : inlines) {
    if (node.type() == type) {
      ++count;
    }
    count += countInline(node.children(), type);
  }
  return count;
}

int countInline(const MarkdownNode& node, InlineType type) {
  int count = countInline(node.inlines(), type);
  for (const auto& child : node.children()) {
    count += countInline(*child, type);
  }
  return count;
}

const InlineNode* findInline(const QVector<InlineNode>& inlines, InlineType type) {
  for (const InlineNode& node : inlines) {
    if (node.type() == type) {
      return &node;
    }
    if (const InlineNode* found = findInline(node.children(), type)) {
      return found;
    }
  }
  return nullptr;
}

const InlineNode* findInline(const MarkdownNode& node, InlineType type) {
  if (const InlineNode* found = findInline(node.inlines(), type)) {
    return found;
  }
  for (const auto& child : node.children()) {
    if (const InlineNode* found = findInline(*child, type)) {
      return found;
    }
  }
  return nullptr;
}

void writeInlines(QDataStream& out, const QVector<InlineNode>& inlines) {
  out << qint32(inlines.size());
  for (const auto& node : inlines) {
    out << qint32(node.type()) << node.text() << node.marker() << node.href() << node.title() << node.alt()
        << node.isAutolink();
    const auto ranges = node.sourceRanges();
    for (const auto& range : {ranges.source, ranges.content, ranges.openMarker, ranges.closeMarker})
      out << qint64(range.start) << qint64(range.end);
    writeInlines(out, node.children());
  }
}

void writeBlock(QDataStream& out, const MarkdownNode& node) {
  const auto range = node.sourceRange();
  out << qint32(node.type()) << node.literal() << qint64(range.byteStart) << qint64(range.byteEnd)
      << qint32(range.lineStart) << qint32(range.lineEnd);
  writeInlines(out, node.inlines());
  out << qint32(node.children().size());
  for (const auto& child : node.children())
    writeBlock(out, *child);
}

// Fixed-seed corpus fingerprint, audited for retained $/$$ metadata and exact
// dollar-run matching. It checks syntax, every inline coordinate and serialization
// independently of the token representation, across all sixteen option settings.
QString delimiterCorpusFingerprint() {
  // Optional binary audit for reviewing intentional parser changes against an
  // older MuffinCore, without replacing the fixed corpus with self-comparison.
  QFile dump(qEnvironmentVariable("MUFFIN_DELIMITER_CORPUS_DUMP"));
  if (!dump.fileName().isEmpty())
    require(dump.open(QIODevice::WriteOnly), "cannot open delimiter corpus audit");
  QDataStream dumpStream(&dump);
  dumpStream.setVersion(QDataStream::Qt_6_0);
  QStringList corpus{QStringLiteral("~~outer ~~inner~~ tail~~"),
                     QStringLiteral("~~a~~b~~c~~"),
                     QStringLiteral("==outer ==inner== tail=="),
                     QStringLiteral("~outer ~inner~ tail~"),
                     QStringLiteral("^outer ^inner^ tail^"),
                     QStringLiteral("~~**b** *i* [l](url)~~"),
                     QStringLiteral("~~a~~~b~~ ~~a~~ ~~ ~~b~~ ~ ~ ===x=== ^^x^^"),
                     QStringLiteral("\\~\\~escaped\\~\\~ \\\\~~plain~~ &#126;&#126;entity&#126;&#126;"),
                     QStringLiteral("~~line\nwrap~~\n\n==next=="),
                     QStringLiteral("~~x~~\\\n~~y~~"),
                     QStringLiteral("| A | B |\n| --- | --- |\n| ~~a~~ | ~b~ ^c^ ==d== |\n"),
                     QStringLiteral("~~![alt](image.png) <kbd>key</kbd> $x$ `code`~~"),
                     QString::fromUtf8("中文~~删除~~ \xf0\x9f\x98\x80==高亮== H~2~O X^2^"),
                     QStringLiteral("~~nested ~~deep ~~inner~~ tail~~ end~~"),
                     QStringLiteral("~~a~~ *unclosed ~~x~~\n\n> ==q==\n> H~2~O\n"),
                     QStringLiteral("~~x~~ ").repeated(2048),
                     QStringLiteral("~~unclosed ").repeated(2048)};
  const QStringList atoms{"a",
                          " b ",
                          "~~",
                          "~",
                          "~~~",
                          "^",
                          "^^",
                          "==",
                          "=",
                          "===",
                          " **b** ",
                          "*i*",
                          "`~~c~~`",
                          "[l](https://example.com/~u)",
                          "![a](image)",
                          "<kbd>k</kbd>",
                          "&#126;",
                          "&amp;",
                          "\\~",
                          "\\^",
                          "\\=",
                          "\\\\",
                          "$x^2$",
                          "\n",
                          "  \n",
                          "\t",
                          QString::fromUtf8("中文"),
                          QString::fromUtf8("\xf0\x9f\x98\x80")};
  quint32 random = 0x19a2b3c4;
  const auto next = [&]() {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    return random;
  };
  for (int sample = 0; sample < 256; ++sample) {
    QString text;
    const int length = 8 + int(next() % 48);
    for (int i = 0; i < length; ++i)
      text += atoms.at(next() % atoms.size());
    if (sample % 4 == 0)
      text.prepend("# ");
    if (sample % 4 == 1)
      text.prepend("> ");
    if (sample % 4 == 2)
      text.prepend("- ");
    corpus.append(std::move(text));
  }
  QCryptographicHash hash(QCryptographicHash::Sha256);
  CmarkGfmParser parser;
  for (const auto& text : corpus) {
    for (int mask = 0; mask < 16; ++mask) {
      ParseOptions options;
      options.enableHighlight = mask & 1;
      options.enableStrikethrough = mask & 2;
      options.enableSubscript = mask & 4;
      options.enableSuperscript = mask & 8;
      const auto parsed = parser.parseDocument(text, options);
      require(parsed.root != nullptr, "delimiter corpus must parse");
      QByteArray bytes;
      QDataStream out(&bytes, QIODevice::WriteOnly);
      out.setVersion(QDataStream::Qt_6_0);
      out << text << qint32(mask);
      writeBlock(out, *parsed.root);
      for (const auto& block : parsed.root->children())
        out << MarkdownSerializer().serializeBlock(*block);
      auto sharedSource = std::make_shared<const QString>(text);
      auto sharedParsed = parser.parseDocumentShared(sharedSource, options);
      require(sharedParsed.root != nullptr, "source-sharing parse must produce a root");
      QByteArray sharedBytes;
      QDataStream sharedOut(&sharedBytes, QIODevice::WriteOnly);
      sharedOut.setVersion(QDataStream::Qt_6_0);
      sharedOut << text << qint32(mask);
      writeBlock(sharedOut, *sharedParsed.root);
      for (const auto& block : sharedParsed.root->children())
        sharedOut << MarkdownSerializer().serializeBlock(*block);
      require(sharedBytes == bytes,
              "direct source slices must preserve the entire delimiter corpus AST and coordinates");
      if (dump.isOpen())
        dumpStream << bytes;
      hash.addData(bytes);
    }
  }
  return QString::fromLatin1(hash.result().toHex());
}
} // namespace

void testSubscriptParses() {
  CmarkGfmParser parser;
  ParseOptions options;
  options.enableSubscript = true;
  ParseResult result = parser.parseDocument(QStringLiteral("H~2~O"), options);
  require(result.root != nullptr, "parse should produce a root");
  require(countInline(*result.root, InlineType::Subscript) == 1, "should find one subscript inline");
  const InlineNode* sub = findInline(*result.root, InlineType::Subscript);
  require(sub != nullptr, "subscript node should exist");
  require(sub->marker() == QStringLiteral("~"), "subscript marker should be ~");
  require(sub->children().size() == 1 && sub->children().first().text() == QStringLiteral("2"),
          "subscript child text should be '2'");
}

void testSuperscriptParses() {
  CmarkGfmParser parser;
  ParseOptions options;
  options.enableSuperscript = true;
  ParseResult result = parser.parseDocument(QStringLiteral("X^2^"), options);
  require(countInline(*result.root, InlineType::Superscript) == 1, "should find one superscript inline");
  const InlineNode* sup = findInline(*result.root, InlineType::Superscript);
  require(sup != nullptr, "superscript node should exist");
  require(sup->marker() == QStringLiteral("^"), "superscript marker should be ^");
  require(sup->children().size() == 1 && sup->children().first().text() == QStringLiteral("2"),
          "superscript child text should be '2'");
}

void testSubscriptNestedFormatting() {
  CmarkGfmParser parser;
  ParseOptions options;
  options.enableSubscript = true;
  ParseResult result = parser.parseDocument(QStringLiteral("~**b**~"), options);
  require(countInline(*result.root, InlineType::Subscript) == 1, "should wrap the bold in a subscript");
  const InlineNode* sub = findInline(*result.root, InlineType::Subscript);
  require(sub != nullptr && countInline(sub->children(), InlineType::Strong) == 1,
          "subscript should contain the strong run");
}

// `~~x~~` is strikethrough (a cmark extension), never subscript — the run-length-1 rule plus cmark
// consuming `~~` first means the two never collide even with both enabled.
void testStrikethroughNotConsumedAsSubscript() {
  CmarkGfmParser parser;
  ParseOptions options;
  options.enableSubscript = true; // enableStrikethrough defaults true
  ParseResult result = parser.parseDocument(QStringLiteral("~~struck~~"), options);
  require(countInline(*result.root, InlineType::Strikethrough) == 1, "~~x~~ should still be strikethrough");
  require(countInline(*result.root, InlineType::Subscript) == 0, "strikethrough must not become subscript");
}

void testSubscriptDisabledStaysLiteral() {
  CmarkGfmParser parser;
  ParseOptions options; // enableSubscript defaults false, enableStrikethrough defaults true
  ParseResult result = parser.parseDocument(QStringLiteral("H~2~O"), options);
  require(countInline(*result.root, InlineType::Subscript) == 0,
          "with enableSubscript=false the markers must stay literal");
  // Guards the single-tilde fix: cmark-gfm's strikethrough extension matches a lone `~`, so without
  // the detach it turned `~2~` into a Strikethrough. We own tilde runs ourselves now, so single `~`
  // must NOT render as strikethrough when subscript is off.
  require(countInline(*result.root, InlineType::Strikethrough) == 0,
          "with subscript off, ~2~ must not be parsed as strikethrough");
}

// Strikethrough is `~~` only (GFM spec); a single `~` is reserved for subscript / literal text.
// This holds regardless of the subscript toggle: `~~x~~` is always strikethrough, `~x~` never is.
void testStrikethroughUsesDoubleTildeOnly() {
  CmarkGfmParser parser;
  ParseOptions options; // subscript off, strikethrough on (defaults)
  ParseResult doubleTilde = parser.parseDocument(QStringLiteral("~~struck~~"), options);
  require(countInline(*doubleTilde.root, InlineType::Strikethrough) == 1,
          "~~x~~ should still parse as strikethrough with subscript off");
  ParseResult singleTilde = parser.parseDocument(QStringLiteral("~plain~"), options);
  require(countInline(*singleTilde.root, InlineType::Strikethrough) == 0,
          "single-tilde ~x~ must not be strikethrough when subscript is off");
}

// A lone `~` flanked by spaces cannot open (its first content char is a space), so `a ~ b ~ c` must
// stay literal — consistent with emphasis/highlight flank rules.
void testSpacedTildeStaysLiteral() {
  CmarkGfmParser parser;
  ParseOptions options;
  options.enableSubscript = true;
  ParseResult result = parser.parseDocument(QStringLiteral("a ~ b ~ c"), options);
  require(countInline(*result.root, InlineType::Subscript) == 0, "a space-flanked ~ must not open a subscript");
}

void testSubscriptSerializesRoundTrip() {
  CmarkGfmParser parser;
  ParseOptions options;
  options.enableSubscript = true;
  ParseResult result = parser.parseDocument(QStringLiteral("H~2~O"), options);
  require(result.root != nullptr, "parse should produce a root");
  require(result.root->children().size() >= 1, "root should have a paragraph child");
  const QString serialized = MarkdownSerializer().serializeBlock(*result.root->children().at(0));
  require(serialized == QStringLiteral("H~2~O"), "subscript should round-trip to H~2~O on serialize");
}

void testDelimiterCandidatesStayLocalAndOpaqueContentStaysLiteral() {
  const QString text = QStringLiteral("~~*struck*~~ H~2~O X^2^ ==mark==\n\n"
                                      "Ordinary **bold** and [label](https://example.com/~user), `~~code~~`.\n\n"
                                      "Entities &#126;&#126;opaque&#126;&#126; and \\~escaped\\~.\n\n"
                                      "| A | B |\n| --- | --- |\n| ~~cell~~ | plain **cell** |\n");
  ParseOptions options;
  options.enableHighlight = options.enableSubscript = options.enableSuperscript = true;
  CmarkGfmParser parser;
  auto result = parser.parseDocument(text, options);
  require(countInline(*result.root, InlineType::Strikethrough) == 2,
          "strike candidates must work across formatting and in tables");
  require(countInline(*result.root, InlineType::Subscript) == 1,
          "opaque code, URL and entity markers must not become subscript");
  require(countInline(*result.root, InlineType::Superscript) == 1, "superscript candidate must remain local");
  require(countInline(*result.root, InlineType::Highlight) == 1, "highlight candidate must remain local");
  const auto& ordinary = *result.root->children().at(1);
  const auto* strong = findInline(ordinary, InlineType::Strong);
  require(strong && strong->children().first().text() == QStringLiteral("bold"),
          "marker-free formatting must be retained");
  const auto r = strong->sourceRange();
  require(text.mid(r.start, r.length()) == QStringLiteral("**bold**"),
          "skipping ordinary text must preserve source coordinates");
}

void testDelimiterCorpusSyntaxAndCoordinates() {
  require(delimiterCorpusFingerprint() ==
              QStringLiteral("f4dab0234037d3db8f23ca5651a1e605daf84476b69db2ced27454bae43bee8b"),
          "delimiter syntax, source coordinates and serialization must match the audited corpus");
}

void testNestedDelimiterSplicingPreservesSourceRanges() {
  const QString text = QStringLiteral("~~outer ~~inner~~ tail~~");
  const auto result = CmarkGfmParser().parseDocument(text, {});
  const auto* outer = findInline(*result.root, InlineType::Strikethrough);
  require(outer && outer->sourceRange().start == 0 && outer->sourceRange().end == text.size(),
          "outer pair must retain its complete source interval");
  const auto* inner = findInline(outer->children(), InlineType::Strikethrough);
  require(inner && text.mid(inner->sourceStart(), inner->sourceEnd() - inner->sourceStart()) == "~~inner~~",
          "nested pair must keep its original marker interval after splicing");
  require(countInline(*result.root, InlineType::Strikethrough) == 2, "nested delimiter pairs must remain nested");
}

int main(int argc, char** argv) {
  QCoreApplication::setOrganizationName(QStringLiteral("MuffinTest"));
  QCoreApplication::setApplicationName(QStringLiteral("SubscriptSuperscriptParseTest"));
  QCoreApplication app(argc, argv);
  if (argc == 2 && QString::fromLocal8Bit(argv[1]) == QStringLiteral("--delimiter-fingerprint")) {
    std::printf("%s\n", qPrintable(delimiterCorpusFingerprint()));
    return 0;
  }
#define RUN_TEST(test) runTest(#test, test)
  RUN_TEST(testSubscriptParses);
  RUN_TEST(testSuperscriptParses);
  RUN_TEST(testSubscriptNestedFormatting);
  RUN_TEST(testStrikethroughNotConsumedAsSubscript);
  RUN_TEST(testStrikethroughUsesDoubleTildeOnly);
  RUN_TEST(testSubscriptDisabledStaysLiteral);
  RUN_TEST(testSpacedTildeStaysLiteral);
  RUN_TEST(testSubscriptSerializesRoundTrip);
  RUN_TEST(testDelimiterCandidatesStayLocalAndOpaqueContentStaysLiteral);
  RUN_TEST(testNestedDelimiterSplicingPreservesSourceRanges);
  RUN_TEST(testDelimiterCorpusSyntaxAndCoordinates);
#undef RUN_TEST
  return 0;
}
