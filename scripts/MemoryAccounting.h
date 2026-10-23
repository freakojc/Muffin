#pragma once

// Untimed diagnostic census. This is payload accounting, not an allocator or
// private-commit profiler. Shared Qt buffers are counted once per census.
#include <QJsonObject>
#include <QSet>

#include "app/MainWindow.h"
#include "diagnostics/ProcessMemory.h"
#include "document/MarkdownDocument.h"
#include "document/SourcePositionIndex.h"
#include "editor/EditorView.h"
#include "render/DecorationPainter.h"
#include "render/ImageLoader.h"
#include "render/TextShadowPainter.h"
#include "theme/CssImageResources.h"
#include "theme/ThemeFontDecoder.h"

namespace muffin::diag {
struct MemoryAccounting {
  QSet<const void*> buffers;
  qint64 strings = 0, sharedSource = 0, arrays = 0, blocks = 0, inlines = 0, metadata = 0;
  qint64 inlineCount = 0, sharedInlineCount = 0, blockCount = 0;
  void string(const QString& text, bool source = false) {
    if (text.capacity() && !buffers.contains(text.constData())) {
      buffers.insert(text.constData());
      (source ? sharedSource : strings) += (text.capacity() + 1) * sizeof(QChar);
    }
  }
  template <class T> void array(const QList<T>& value) {
    if (value.capacity() && !buffers.contains(value.constData())) {
      buffers.insert(value.constData());
      arrays += value.capacity() * sizeof(T);
    }
  }
  void text(const PieceTable& value) {
    string(value.original_, true);
    string(value.changes_, true);
    array(value.originalNewlines_);
    array(value.changesNewlines_);
    array(value.originalWordStarts_);
    array(value.changesWordStarts_);
    arrays += value.pieces_.capacity() * sizeof(PieceTable::Piece);
    arrays += (value.prefix_.capacity() + value.prefixNewlines_.capacity()) * sizeof(qint64);
  }
  void inlineNode(const InlineNode& node) {
    ++inlineCount;
    if (const auto* slice = std::get_if<InlineNode::SharedTextSlice>(&node.text_)) {
      ++sharedInlineCount;
      string(*slice->source, true);
    } else
      string(std::get<QString>(node.text_));
    if (node.extendedMetadata_) {
      metadata += sizeof(InlineNode::ExtendedMetadata);
      const auto& m = *node.extendedMetadata_;
      string(m.marker);
      string(m.href);
      string(m.title);
      string(m.alt);
      if (m.wideRanges)
        metadata += sizeof(InlineSourceRanges);
    }
    array(node.children_);
    for (const auto& child : node.children_)
      inlineNode(child);
  }
  void block(const MarkdownNode& node) {
    ++blockCount;
    blocks += sizeof(MarkdownNode);
    arrays += node.children_.capacity() * sizeof(std::unique_ptr<MarkdownNode>);
    array(node.metadata_.inlines);
    string(node.metadata_.literal);
    if (node.metadata_.extended) {
      metadata += sizeof(MarkdownNode::ExtendedMetadata);
      const auto& m = *node.metadata_.extended;
      string(m.codeLanguage);
      array(m.tableAlignments);
      if (m.definition)
        metadata += sizeof(DefinitionBlock);
    }
    for (const auto& child : node.metadata_.inlines)
      inlineNode(child);
    for (const auto& child : node.children_)
      block(*child);
  }
  QJsonObject document(const MarkdownDocument& doc) {
    text(doc.pieceText());
    const auto textStrings = sharedSource, textArrays = arrays;
    block(doc.root());
    return {{"block_nodes", blockCount},
            {"inline_nodes", inlineCount},
            {"shared_inline_nodes", sharedInlineCount},
            {"sizeof_inline", qint64(sizeof(InlineNode))},
            {"text_buffer_payload", textStrings},
            {"text_index_payload", textArrays},
            {"ast_block_payload", blocks},
            {"ast_arrays_payload", arrays - textArrays},
            {"ast_metadata_payload", metadata},
            {"ast_string_payload", strings},
            {"ast_extra_source_payload", sharedSource - textStrings},
            {"source_position_payload", qint64(doc.root().children().size() * sizeof(SourcePositionToken))},
            {"payload_total", blocks + arrays + metadata + strings + sharedSource}};
  }
  static QJsonObject layout(const EditorView& view) {
    const auto* value = view.layout_.get();
    if (!value)
      return {};
    return {{"slot_count", qint64(value->slots_.size())},
            {"slot_payload", qint64(value->slots_.capacity() * sizeof(DocumentLayout::BlockSlot))},
            {"position_payload", qint64(value->positionIndex_.size() * sizeof(LayoutPositionToken))},
            {"resident_details", value->residentDetailCount()},
            {"detail_object_minimum", qint64(value->residentDetailCount() * sizeof(BlockLayout))},
            {"node_style_entries", view.theme_.nodeStyleCache_.size()},
            {"inline_style_entries", view.theme_.inlineStyleCache_.size()}};
  }
  static QJsonObject process() {
    QJsonObject result{{"working_set_bytes", workingSetBytes()}};
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
                             sizeof(counters))) {
      result["private_commit_bytes"] = qint64(counters.PrivateUsage);
      result["peak_working_set_bytes"] = qint64(counters.PeakWorkingSetSize);
    }
#endif
    return result;
  }
  static QJsonObject resources(const MainWindow& window, const QVector<DocumentTab*>& tabs) {
    const auto font = themeFontRegistrationStats();
    const auto box = DecorationPainter::boxShadowRasterStats();
    const auto text = textShadowRasterStats();
    const auto draft = window.draftWriter_.queueStats();
    auto& images = ImageLoader::instance();
    qint64 imageBytes = 0, cssBytes = 0;
    QSet<const void*> sources;
    for (const auto& image : images.cache_) {
      if (!image.isNull() && !sources.contains(image.constBits())) {
        sources.insert(image.constBits());
        imageBytes += image.sizeInBytes();
      }
    }
    qsizetype cssCaches = 0;
    for (auto* tab : tabs) {
      auto sheet = tab->renderView->theme().documentStyleSheet();
      if (sheet && sheet->imageResources() && !sources.contains(sheet->imageResources().get())) {
        const auto* resource = sheet->imageResources().get();
        sources.insert(resource);
        ++cssCaches;
        cssBytes += resource->cache_.totalCost();
      }
    }
    return {{"private_font_faces", font.faces},
            {"native_font_bytes_submitted_to_qt", font.nativeBytesSubmitted},
            {"image_cache_bytes", imageBytes},
            {"css_source_cache_cost_bytes", cssBytes},
            {"css_source_cache_count", cssCaches},
            {"svg_raster_budget_per_source_cache", 16 * 1024 * 1024},
            {"box_shadow_cache_bytes", box.cacheBytes},
            {"text_shadow_cache_bytes", text.cacheBytes},
            {"draft_pending", draft.pending},
            {"draft_running", draft.running},
            {"draft_pending_code_units", draft.pendingCodeUnits},
            {"draft_running_code_units", draft.runningCodeUnits},
            {"font_method",
             "Cumulative bytes submitted to native Qt font registration; OS shaping/cache residency is not exposed."}};
  }
};
} // namespace muffin::diag
