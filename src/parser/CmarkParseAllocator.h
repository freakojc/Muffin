#pragma once

#include <array>
#include <cstddef>

extern "C" {
#include "cmark-gfm.h"
}

namespace muffin {

// One parser/tree lifetime, confined to one thread at a time. Empty pages are
// reclaimed during conversion, so the cmark tree need not overlap the full
// Muffin AST. This is deliberately independent of cmark's global arena.
class CmarkParseAllocator {
public:
  struct Statistics {
    size_t liveAllocations = 0;
    size_t reservedBytes = 0;
    size_t peakReservedBytes = 0;
  };

  // cmark_mem has no context argument. Scope routes only NEW allocations;
  // realloc/free recover their owner from the allocation, even in another
  // nested scope or after a sequential handoff to a different thread.
  class Scope {
  public:
    explicit Scope(CmarkParseAllocator& allocator) noexcept;
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    CmarkParseAllocator* previous_;
    CmarkParseAllocator* allocator_;
  };

  CmarkParseAllocator() = default;
  ~CmarkParseAllocator();
  CmarkParseAllocator(const CmarkParseAllocator&) = delete;
  CmarkParseAllocator& operator=(const CmarkParseAllocator&) = delete;

  cmark_mem* memory() noexcept { return &memory_; }
  const Statistics& statistics() const noexcept { return statistics_; }
  // Release the one spare empty page retained per size class. Live allocations
  // remain valid; destruction requires all cmark objects to have been freed.
  void trim() noexcept;

private:
  struct Page;
  struct Header;
  static constexpr size_t classCount = 23;
  static thread_local CmarkParseAllocator* active_;

  static void* callocCallback(size_t count, size_t size) noexcept;
  static void* reallocCallback(void* memory, size_t size) noexcept;
  static void freeCallback(void* memory) noexcept;
  void* allocate(size_t requested, bool zero) noexcept;
  Page* newPage(size_t group, size_t size) noexcept;
  void releasePage(Page* page) noexcept;
  void addAvailable(Page* page) noexcept;
  void removeAvailable(Page* page) noexcept;

  cmark_mem memory_{callocCallback, reallocCallback, freeCallback};
  std::array<Page*, classCount> available_{};
  std::array<Page*, classCount> spare_{};
  Page* pages_ = nullptr;
  Statistics statistics_;
};

} // namespace muffin
