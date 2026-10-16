#pragma once

#include "document/MarkdownNode.h"

#include <QString>

extern "C" {
#include "cmark-gfm.h"
}

namespace muffin {

class LineStartOffsetCache;

class CmarkNodeAdapter {
public:
  CmarkNodeAdapter() = default;
  CmarkNodeAdapter(const LineStartOffsetCache* lineOffsets, QStringView markdown,
                   std::shared_ptr<const QString> sharedSource = {}, qsizetype sourceBase = 0);

  std::unique_ptr<MarkdownNode> convertBlock(cmark_node* node);
  InlineNode convertInline(cmark_node* node);

  // Aggregate convertBlock breakdown (QString copies vs inline-source annotation), reported via
  // muffin.perf. No-op unless enabled.
  static void setPerfEnabled(bool enabled);
  static void resetPerfCounters();
  static void dumpConvertBreakdown();

private:
  BlockType mapBlockType(cmark_node* node) const;
  InlineType mapInlineType(cmark_node* node) const;
  SourceRange readSourceRange(cmark_node* node) const;
  QVector<InlineNode> convertInlineChildren(cmark_node* node);
  void annotateInlineSource(cmark_node* cmarkNode, InlineNode& inlineNode) const;
  bool trySharedLiteral(cmark_node* node, InlineNode& result) const;
  qsizetype sourceOffset(int line, int byteColumn) const;
  void readBlockMetadata(cmark_node* cmarkNode, MarkdownNode& muffinNode);
  void readTableMetadata(cmark_node* cmarkNode, MarkdownNode& muffinNode);

  const LineStartOffsetCache* lineOffsets_ = nullptr;
  QStringView markdown_;
  std::shared_ptr<const QString> sharedSource_;
  qsizetype sourceBase_ = 0;
  struct ByteCheckpoint { qsizetype column, offset; };
  // Parser-local scratch, never a mutable cache on the shared document index.
  // One checkpoint per 64 UTF-16 units bounds memory even for a huge line.
  mutable int cachedLine_ = -1;
  mutable bool cachedLineAscii_ = true;
  mutable QStringView cachedLineText_;
  mutable QVector<ByteCheckpoint> byteCheckpoints_;
};

}  // namespace muffin
