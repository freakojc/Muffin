#include "parser/CmarkParseAllocator.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace muffin {
namespace {
constexpr size_t pageBytes = 256 * 1024;
constexpr std::array<size_t, 23> sizes{
    16, 32, 48, 64, 80, 96, 112, 128, 160, 176, 192, 224,
    256, 320, 384, 448, 512, 768, 1024, 1536, 2048, 3072, 4096};
// Immutable lookup shared by all instances; no per-parse table construction.
constexpr auto sizeGroups = [] {
  std::array<uint8_t, 257> result{};
  for (size_t i = 0; i < result.size(); ++i) {
    size_t group = 0;
    while (sizes[group] < std::max<size_t>(i * 16, 1)) ++group;
    result[i] = static_cast<uint8_t>(group);
  }
  return result;
}();
}  // namespace

struct alignas(std::max_align_t) CmarkParseAllocator::Header {
  Page* page;
  size_t requested;
};

struct alignas(std::max_align_t) CmarkParseAllocator::Page {
  CmarkParseAllocator* owner;
  Page* previous = nullptr;
  Page* next = nullptr;
  Page* availablePrevious = nullptr;
  Page* availableNext = nullptr;
  void* freeHead = nullptr;
  size_t slotSize, capacity, bytes, group;
  size_t used = 0, live = 0;
  bool available = false;
};

thread_local CmarkParseAllocator* CmarkParseAllocator::active_ = nullptr;

CmarkParseAllocator::Scope::Scope(CmarkParseAllocator& allocator) noexcept
    : previous_(std::exchange(active_, &allocator)), allocator_(&allocator) {}

CmarkParseAllocator::Scope::~Scope() {
  assert(active_ == allocator_);
  active_ = previous_;
}

CmarkParseAllocator::~CmarkParseAllocator() {
  assert(statistics_.liveAllocations == 0);
  while (pages_) releasePage(pages_);
}

void CmarkParseAllocator::addAvailable(Page* page) noexcept {
  assert(!page->available && page->group < classCount);
  auto*& head = available_[page->group];
  page->availableNext = head;
  page->availablePrevious = nullptr;
  if (head) head->availablePrevious = page;
  head = page;
  page->available = true;
}

void CmarkParseAllocator::removeAvailable(Page* page) noexcept {
  assert(page->available);
  if (page->availablePrevious) page->availablePrevious->availableNext = page->availableNext;
  else available_[page->group] = page->availableNext;
  if (page->availableNext) page->availableNext->availablePrevious = page->availablePrevious;
  page->availableNext = page->availablePrevious = nullptr;
  page->available = false;
}

CmarkParseAllocator::Page* CmarkParseAllocator::newPage(size_t group, size_t size) noexcept {
  static_assert(sizes.size() == classCount);
  const size_t payload = group == classCount ? size : sizes[group];
  constexpr size_t alignment = alignof(std::max_align_t);
  if (payload > std::numeric_limits<size_t>::max() - sizeof(Header) - sizeof(Page) - alignment) {
    std::abort();  // Match cmark's fail-fast allocator policy, including overflow.
  }
  const size_t stride = (sizeof(Header) + payload + alignment - 1) / alignment * alignment;
  const size_t capacity = group == classCount ? 1 : (pageBytes - sizeof(Page)) / stride;
  const size_t bytes = sizeof(Page) + stride * capacity;
  auto* memory = std::malloc(bytes);
  if (!memory) std::abort();
  auto* page = new (memory) Page{};
  page->owner = this;
  page->slotSize = stride;
  page->capacity = capacity;
  page->bytes = bytes;
  page->group = group;
  page->next = pages_;
  if (pages_) pages_->previous = page;
  pages_ = page;
  statistics_.reservedBytes += bytes;
  statistics_.peakReservedBytes = std::max(statistics_.peakReservedBytes, statistics_.reservedBytes);
  if (group < classCount) addAvailable(page);
  return page;
}

void CmarkParseAllocator::releasePage(Page* page) noexcept {
  assert(page->live == 0);
  if (page->available) removeAvailable(page);
  if (page->group < classCount && spare_[page->group] == page) spare_[page->group] = nullptr;
  if (page->previous) page->previous->next = page->next;
  else pages_ = page->next;
  if (page->next) page->next->previous = page->previous;
  statistics_.reservedBytes -= page->bytes;
  std::free(page);
}

void* CmarkParseAllocator::allocate(size_t requested, bool zero) noexcept {
  const size_t size = std::max<size_t>(requested, 1);
  const size_t group = size > sizes.back() ? classCount : sizeGroups[(size + 15) / 16];
  Page* page = group == classCount ? newPage(group, size) : available_[group];
  if (!page) page = newPage(group, size);
  Header* header;
  if (page->freeHead) {
    header = static_cast<Header*>(page->freeHead) - 1;
    page->freeHead = *static_cast<void**>(page->freeHead);
  } else {
    auto* slot = reinterpret_cast<unsigned char*>(page + 1) + page->used++ * page->slotSize;
    header = new (slot) Header{page, requested};
  }
  header->requested = requested;
  if (group < classCount && spare_[group] == page) spare_[group] = nullptr;
  ++page->live;
  ++statistics_.liveAllocations;
  if (page->available && !page->freeHead && page->used == page->capacity) removeAvailable(page);
  void* result = header + 1;
  if (zero) std::memset(result, 0, size);
  return result;
}

void* CmarkParseAllocator::callocCallback(size_t count, size_t size) noexcept {
  if (!active_ || (size && count > std::numeric_limits<size_t>::max() / size)) std::abort();
  return active_->allocate(count * size, true);
}

void CmarkParseAllocator::freeCallback(void* memory) noexcept {
  if (!memory) return;
  auto* header = static_cast<Header*>(memory) - 1;
  auto* page = header->page;
  auto* owner = page->owner;
  assert(page->live > 0 && owner->statistics_.liveAllocations > 0);
  --owner->statistics_.liveAllocations;
  --page->live;
  if (page->group == classCount) {
    owner->releasePage(page);
    return;
  }
  *static_cast<void**>(memory) = page->freeHead;
  page->freeHead = memory;
  if (!page->available) owner->addAvailable(page);
  if (page->live == 0) {
    if (owner->spare_[page->group]) owner->releasePage(page);
    else owner->spare_[page->group] = page;
  }
}

void* CmarkParseAllocator::reallocCallback(void* memory, size_t size) noexcept {
  if (!memory) {
    if (!active_) std::abort();
    return active_->allocate(size, false);
  }
  auto* header = static_cast<Header*>(memory) - 1;
  auto* owner = header->page->owner;
  if (size <= header->page->slotSize - sizeof(Header)) {
    header->requested = size;
    return memory;
  }
  void* result = owner->allocate(size, false);
  std::memcpy(result, memory, std::min(header->requested, size));
  freeCallback(memory);
  return result;
}

void CmarkParseAllocator::trim() noexcept {
  for (auto* page : spare_) if (page) releasePage(page);
}

}  // namespace muffin
