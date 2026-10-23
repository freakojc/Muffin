#include "app/NoteLinks.h"
#include "document/ImageSyntaxOps.h"
#include "document/LinkSyntaxOps.h"
#include "editor/ResourceUrl.h"
#include "io/FileController.h"
#include "io/FilePathOps.h"
#include "parser/CmarkGfmParser.h"
#include <QFile>
#include <QSet>
#include <algorithm>

namespace {
QString headingSlug(const QString& title) {
  QString result;
  for (const auto c : title.toLower()) {
    if (c.isSpace())
      result += '-';
    else if (c.isLetterOrNumber() || c == '-' || c == '_' || c.isMark() || c.isSurrogate())
      result += c;
  }
  return result;
}
QString movedHref(const QString& href, const QString& source, const QString& oldPath, const QString& newPath) {
  if (href.isEmpty() || href.startsWith('#'))
    return {};
  auto url = muffin::resolvedUrlForDocumentResource(href, source);
  if (!url.isLocalFile())
    return {};
  const auto target = QFileInfo(url.toLocalFile()).absoluteFilePath();
  const auto movedTarget = muffin::FilePathOps::remapDescendant(target, oldPath, newPath);
  const auto movedSource = muffin::FilePathOps::remapDescendant(source, oldPath, newPath);
  const QUrl authored(href);
  const bool relative = authored.scheme().isEmpty() && !QFileInfo(authored.path()).isAbsolute();
  if (relative) {
    const auto before = QDir(QFileInfo(source).absolutePath()).relativeFilePath(target);
    const auto after = QDir(QFileInfo(movedSource).absolutePath()).relativeFilePath(movedTarget);
    if (before == after)
      return {};
    url = authored;
    url.setPath(after);
  } else {
    if (movedTarget == target)
      return {};
    if (authored.isLocalFile())
      url = QUrl::fromLocalFile(movedTarget);
    else {
      url = QUrl();
      url.setPath(movedTarget);
    }
    url.setQuery(authored.query(QUrl::FullyEncoded), QUrl::StrictMode);
    url.setFragment(authored.fragment(QUrl::FullyEncoded), QUrl::StrictMode);
  }
  return url.toString(QUrl::FullyEncoded);
}
struct Edit {
  qsizetype start, end;
  QString value;
};
// A label can contain code such as `[x](example.md)`. Validate the complete
// destination suffix with the Markdown parser so bracket-like label text or
// titles can never be mistaken for the address being rewritten.
muffin::image_syntax::SourceLocation destinationLocation(const QString& snippet, const QString& href) {
  qsizetype position = snippet.size();
  while ((position = snippet.lastIndexOf(QStringLiteral("]("), position - 1)) >= 0) {
    const QString candidate = QStringLiteral("![x]") + snippet.mid(position + 1);
    muffin::CmarkGfmParser parser;
    const auto parsed = parser.parseDocument(candidate, {});
    if (!parsed.root || parsed.root->children().empty())
      continue;
    const auto& paragraph = *parsed.root->children().front();
    if (paragraph.inlines().size() != 1)
      continue;
    const auto& image = paragraph.inlines().first();
    if (image.type() != muffin::InlineType::Image || image.href() != href ||
        image.sourceRange().end + paragraph.inlineAbsoluteDelta() != candidate.size())
      continue;
    const auto location = muffin::image_syntax::findSource(candidate);
    if (location.found)
      return {true, position + 1 + location.start - 4, position + 1 + location.end - 4};
  }
  return {};
}
}

bool muffin::NoteLinks::isMarkdownPath(const QString& path) {
  static const QSet<QString> extensions{"md", "markdown", "mdown", "mkd", "mkdn", "mdwn"};
  return extensions.contains(QFileInfo(path).suffix().toLower());
}

muffin::NodeId muffin::NoteLinks::headingTarget(const QVector<OutlineEntry>& outline, const QString& fragment) {
  QSet<QString> used;
  for (const auto& entry : outline) {
    const auto base = headingSlug(entry.title);
    QString slug = base;
    int suffix = 0;
    while (used.contains(slug))
      slug = base + '-' + QString::number(++suffix);
    used.insert(slug);
    if (slug == fragment)
      return entry.nodeId;
  }
  // Also accept the heading as written, including spaces and punctuation.
  for (const auto& entry : outline)
    if (entry.title == fragment)
      return entry.nodeId;
  return {};
}

QString muffin::NoteLinks::rewrite(const QString& text, const QString& sourcePath, const QString& oldPath,
                                   const QString& newPath, int* count) {
  QVector<Edit> edits;
  CmarkGfmParser parser;
  const auto parsed = parser.parseDocument(text, {});
  const auto add = [&](qsizetype start, qsizetype end, const QString& href, bool bracketed) {
    if (start < 0 || end < start || end > text.size())
      return;
    const auto replacement = movedHref(href, sourcePath, oldPath, newPath);
    if (replacement.isEmpty())
      return;
    const auto value = link_syntax::destination(replacement, bracketed);
    if (text.mid(start, end - start) != value)
      edits.append({start, end, value});
  };
  const auto inlines = [&](auto&& self, const QVector<InlineNode>& nodes, qsizetype base) -> void {
    for (const auto& node : nodes) {
      if ((node.type() == InlineType::Link || node.type() == InlineType::Image) && !node.isAutolink()) {
        const auto range = node.sourceRange();
        if (range.isValid() && range.end + base <= text.size()) {
          const auto snippet = text.mid(base + range.start, range.length());
          const auto location = movedHref(node.href(), sourcePath, oldPath, newPath).isEmpty()
                                    ? image_syntax::SourceLocation{}
                                    : destinationLocation(snippet, node.href());
          if (location.found) {
            const auto start = base + range.start + location.start;
            const auto end = base + range.start + location.end;
            add(start, end, node.href(), text.at(start) == '<');
          }
        }
      }
      self(self, node.children(), base);
    }
  };
  const auto blocks = [&](auto&& self, const MarkdownNode& node) -> void {
    const auto definition = node.definition();
    if (definition.kind == DefinitionBlock::Kind::Link && definition.destinationRange.isValid()) {
      auto range = definition.destinationRange;
      // The parser's field excludes angle delimiters; replace them together.
      const bool angle = definition.destinationDelimiter == DefinitionBlock::DestinationDelimiter::Angle;
      if (angle) {
        --range.start;
        ++range.end;
      }
      const auto image = image_syntax::parse(QStringLiteral("![x](%1)").arg(text.mid(range.start, range.length())));
      if (image.syntax == image_syntax::Syntax::Markdown)
        add(range.start, range.end, image.src, angle);
    }
    inlines(inlines, node.inlines(), node.inlineAbsoluteDelta());
    for (const auto& child : node.children())
      self(self, *child);
  };
  if (parsed.root)
    blocks(blocks, *parsed.root);
  std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.start < b.start; });
  QString result = text;
  qsizetype boundary = text.size();
  int changed = 0;
  for (auto it = edits.crbegin(); it != edits.crend(); ++it) {
    if (it->end > boundary)
      continue;
    result.replace(it->start, it->end - it->start, it->value);
    boundary = it->start;
    ++changed;
  }
  if (count)
    *count = changed;
  return result;
}

muffin::NoteLinkPlan muffin::NoteLinks::prepare(const QString& root, const QString& oldPath, const QString& newPath,
                                                const OpenSearchBuffers& buffers, const SearchCancellation& cancel) {
  NoteLinkPlan plan;
  plan.root = root;
  auto index = WorkspaceFileIndex::scan(root, {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn"}, cancel);
  // Always inspect the moved note itself, even if it lives in an excluded build directory.
  if (QFileInfo(oldPath).isFile() && isMarkdownPath(oldPath) && !index.paths.contains(oldPath))
    index.paths.append(oldPath);
  if (QFileInfo(oldPath).isDir() && oldPath != root) {
    const auto moved = WorkspaceFileIndex::scan(oldPath, {"md", "markdown", "mdown", "mkd", "mkdn", "mdwn"}, cancel);
    index.paths.append(moved.paths);
    index.paths.removeDuplicates();
    index.unreadableDirectories.append(moved.unreadableDirectories);
    index.truncated = index.truncated || moved.truncated;
  }
  plan.complete = !index.truncated;
  plan.skipped = index.unreadableDirectories;
  qint64 retained = 0;
  for (const auto& path : index.paths) {
    if (cancel && cancel->load()) {
      plan.complete = false;
      break;
    }
    QFile input(path);
    const auto buffer = buffers.constFind(WorkspaceFileIndex::pathKey(path));
    if (QFileInfo(path).isSymLink() || QFileInfo(path).isJunction() || !input.open(QIODevice::ReadOnly) ||
        input.size() > 8 * 1024 * 1024 ||
        (buffer != buffers.cend() && (buffer->busy || buffer->snapshot || buffer->text.size() > 4 * 1024 * 1024))) {
      plan.skipped.append(path);
      continue;
    }
    auto bytes = input.readAll();
    QString diskText;
    TextFileFormat format;
    QByteArray roundTrip;
    if (input.error() != QFileDevice::NoError || !FileController::decodeTextBytes(bytes, &diskText, &format) ||
        diskText.contains(QChar(0)) || !FileController::encodeTextBytes(diskText, format, &roundTrip) ||
        roundTrip != bytes || (buffer != buffers.cend() && !buffer->modified && buffer->text != diskText)) {
      plan.skipped.append(path);
      continue;
    }
    const bool unsaved = buffer != buffers.cend() && buffer->modified;
    const auto before = unsaved ? buffer->text : diskText;
    int matches = 0;
    auto after = rewrite(before, path, oldPath, newPath, &matches);
    if (matches == 0)
      continue;
    QByteArray encoded;
    if (!FileController::encodeTextBytes(after, format, &encoded) || !QFileInfo(path).isWritable()) {
      plan.skipped.append(path);
      continue;
    }
    retained += bytes.size() + encoded.size() + (before.size() + after.size()) * 2;
    if (retained > 64 * 1024 * 1024) {
      plan.complete = false;
      break;
    }
    plan.changes.append({{path, before, std::move(after), std::move(bytes), std::move(encoded), matches}, unsaved});
  }
  return plan;
}
