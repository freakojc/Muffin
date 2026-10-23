#include "parser/CmarkParseAllocator.h"
#include "parser/CmarkGfmParser.h"
#include "document/ImageSyntaxOps.h"
#include "ParserTestUtils.h"

#include <QDataStream>
#include <QIODevice>
#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cstdint>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

using namespace muffin;

namespace {
void testAllocationContract(unsigned seed) {
  CmarkParseAllocator owner, nested;
  struct Item {
    void* data = nullptr;
    size_t size = 0;
    unsigned char value = 0;
  };
  std::vector<Item> items(500);
  std::mt19937 random(seed);
  const size_t sizes[]{0, 1, 15, 16, 17, 31, 32, 64, 175, 176, 177, 255, 256, 1023, 4095, 4096, 4097, 65536};
  {
    CmarkParseAllocator::Scope scope(owner);
    for (int step = 0; step < 30000; ++step) {
      auto& item = items[random() % items.size()];
      if (!item.data) {
        item.size = sizes[random() % std::size(sizes)];
        item.data = owner.memory()->calloc(item.size, 1);
        require(reinterpret_cast<uintptr_t>(item.data) % alignof(std::max_align_t) == 0,
                "cmark allocation alignment changed");
        for (size_t i = 0; i < item.size; ++i)
          require(static_cast<unsigned char*>(item.data)[i] == 0, "cmark calloc must clear reused slots");
        item.value = static_cast<unsigned char>(random());
        std::memset(item.data, item.value, item.size);
      } else {
        for (size_t i = 0; i < item.size; ++i)
          require(static_cast<unsigned char*>(item.data)[i] == item.value, "cmark live allocation was overwritten");
        if (random() % 3 == 0) {
          owner.memory()->free(item.data);
          item = {};
        } else {
          const auto size = sizes[random() % std::size(sizes)];
          CmarkParseAllocator::Scope otherScope(nested);
          item.data = nested.memory()->realloc(item.data, size);
          for (size_t i = 0; i < std::min(item.size, size); ++i)
            require(static_cast<unsigned char*>(item.data)[i] == item.value, "cmark realloc lost existing bytes");
          item.size = size;
          std::memset(item.data, item.value, size);
        }
      }
    }
    // realloc(nullptr) routes to the active instance, not a previous nested one.
    void* fresh = owner.memory()->realloc(nullptr, 176);
    require(owner.statistics().liveAllocations > 0 && nested.statistics().liveAllocations == 0,
            "Nested scope failed to restore the allocator");
    owner.memory()->free(fresh);
  }
  // Existing allocations do not require an active scope and recover their owner.
  for (auto& item : items)
    if (item.data)
      nested.memory()->free(item.data);
  require(owner.statistics().liveAllocations == 0 && nested.statistics().liveAllocations == 0,
          "cmark allocation survived final free");
  owner.trim();
  nested.trim();
  require(owner.statistics().reservedBytes == 0 && nested.statistics().reservedBytes == 0,
          "cmark empty pages survived trim");

  void* transferred = nullptr;
  std::thread allocate([&] {
    CmarkParseAllocator::Scope scope(owner);
    transferred = owner.memory()->calloc(1, 176);
  });
  allocate.join();
  std::thread release([&] { nested.memory()->free(transferred); });
  release.join();
  owner.trim();
  require(owner.statistics().liveAllocations == 0 && owner.statistics().reservedBytes == 0,
          "Sequential thread handoff leaked a cmark page");
}

void testEmptyPagesReclaimedBeforeParseEnd() {
  CmarkParseAllocator allocator;
  CmarkParseAllocator::Scope scope(allocator);
  std::vector<void*> nodes;
  for (int i = 0; i < 12000; ++i)
    nodes.push_back(allocator.memory()->calloc(1, 176));
  const auto peak = allocator.statistics().reservedBytes;
  std::memset(nodes.back(), 0x5a, 176);
  for (size_t i = 0; i < nodes.size() / 2; ++i)
    allocator.memory()->free(nodes[i]);
  allocator.trim();
  require(allocator.statistics().reservedBytes < peak && allocator.statistics().liveAllocations == 6000,
          "Consumed cmark pages must be reclaimed while other nodes remain live");
  require(static_cast<unsigned char*>(nodes.back())[175] == 0x5a, "trim changed a live cmark allocation");
  for (size_t i = nodes.size() / 2; i < nodes.size(); ++i)
    allocator.memory()->free(nodes[i]);
  allocator.trim();
  require(allocator.statistics().reservedBytes == 0, "All cmark pages must be returned after conversion");
}

void snapshotInlines(QDataStream& out, const QVector<InlineNode>& nodes) {
  out << qint64(nodes.size());
  for (const auto& node : nodes) {
    out << qint32(node.type()) << node.text() << node.marker() << node.href() << node.title() << node.alt()
        << node.isAutolink() << node.usesSharedText();
    const auto r = node.sourceRanges();
    for (const auto& range : {r.source, r.content, r.openMarker, r.closeMarker})
      out << qint64(range.start) << qint64(range.end);
    snapshotInlines(out, node.children());
  }
}

void snapshotBlock(QDataStream& out, const MarkdownNode& node) {
  const auto r = node.sourceRange();
  out << qint32(node.type()) << node.literal() << node.codeLanguage() << node.headingLevel() << qint32(node.listKind())
      << node.listStart() << node.listTight() << node.isTaskItem() << node.taskChecked() << qint64(r.byteStart)
      << qint64(r.byteEnd) << r.lineStart << r.lineEnd << r.columnStart << r.columnEnd;
  snapshotInlines(out, node.inlines());
  out << qint64(node.children().size());
  for (const auto& child : node.children())
    snapshotBlock(out, *child);
}

QByteArray snapshot(const MarkdownNode& root) {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  snapshotBlock(stream, root);
  return bytes;
}

void testIndependentConcurrentParses() {
  const QString markdown =
      QString::fromUtf8("# 中文 title\n\ntext $x$ and $$y^2$$ **bold** ~~strike~~ `code` &amp;.\n\n"
                        "| a | b |\n|---|---|\n| $z$ | [^a] |\n\n"
                        "- [x] task\n- item https://example.com\n\n"
                        "[^b]: Other [^a].\n\n[^a]: Note.\n\nuse [^b].\n")
          .repeated(12);
  constexpr int workers = 4;
  std::array<ParseOptions, workers> options;
  options[1].enableMath = false;
  options[2].relaxedInlineMath = false;
  options[3].enableAutolink = options[3].enableTable = options[3].enableTaskList = false;
  const auto shared = std::make_shared<const QString>(markdown);
  std::array<QByteArray, workers> expected;
  CmarkGfmParser reference;
  for (int i = 0; i < workers; ++i) {
    const auto parsed = reference.parseDocumentShared(shared, options[i]);
    require(bool(parsed.root), "Concurrent parse reference failed");
    expected[i] = snapshot(*parsed.root);
  }
  std::barrier start(workers);
  std::atomic<bool> matches = true;
  std::vector<std::thread> threads;
  for (int i = 0; i < workers; ++i)
    threads.emplace_back([&, i] {
      CmarkGfmParser parser;
      for (int repeat = 0; repeat < 100; ++repeat) {
        start.arrive_and_wait();
        const auto parsed = parser.parseDocumentShared(shared, options[i]);
        if (!parsed.root || snapshot(*parsed.root) != expected[i])
          matches = false;
        // The plain cmark image parser shares cmark's globals with document
        // parsers even though it attaches no extensions.
        const auto image = image_syntax::parse(QStringLiteral("![a $x$ &amp; **bold**](image.png \"title\")"));
        if (image.src != QStringLiteral("image.png") || image.alt != QStringLiteral("a $x$ & bold") ||
            image.title != QStringLiteral("title"))
          matches = false;
      }
    });
  for (auto& thread : threads)
    thread.join();
  require(matches, "Concurrent parsers changed math, extensions, text or source coordinates");

  // Local reparses use the same allocator lifetime as complete documents.
  const auto local = reference.parseBlock(markdown, BlockType::Paragraph, options[0]);
  const auto unshared = reference.parseDocument(markdown, options[0]);
  require(local.root && unshared.root && snapshot(*local.root) == snapshot(*unshared.root),
          "Local and complete parses diverged after allocator integration");
}
} // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  testEmptyPagesReclaimedBeforeParseEnd();
  testAllocationContract(1);
  std::vector<std::thread> threads;
  for (unsigned i = 0; i < 4; ++i)
    threads.emplace_back([i] { testAllocationContract(100 + i); });
  for (auto& thread : threads)
    thread.join();
  testIndependentConcurrentParses();
  return 0;
}
