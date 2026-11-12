// A v8::PageAllocator that puts V8's regular heap pages on transparent huge
// pages.
//
// V8 reserves each regular heap page (256 KiB, 256 KiB-aligned) with its own
// mmap() call and frees it with munmap(). Pages reserved and touched one at a
// time never get a huge page when they are first touched, so a large heap runs
// almost entirely on 4 KiB pages and pays for it in TLB misses.
//
// With NODE_HEAP_HUGEPAGES=1 this allocator serves those page requests out of
// large arenas that are mapped read-write once, 2 MiB-aligned and marked
// MADV_HUGEPAGE, so the first touch of a 2 MiB region faults in one huge page.
// Every other request (the code range, large objects, read-only or executable
// memory, other sizes) goes to V8's own allocator unchanged.
//
// Inside an arena the contract of v8::PageAllocator is kept:
//  - SetPermissions(kReadWrite) on a block that has never been changed is a
//    no-op, because the block is already mapped read-write. Every other
//    permission change is applied with mprotect(), as V8's allocator does
//    (including the discard that V8 does for kNoAccess).
//  - DecommitPages() and ReleasePages() map the range PROT_NONE, so it faults
//    on access and reads back as zero when recommitted, as with V8's
//    allocator.
//  - DiscardSystemPages() is a hint; it is applied unless
//    NODE_HEAP_HUGEPAGES_DISCARD=0.
//  - FreePages() returns the block to the arena. Once all eight blocks of a
//    2 MiB region are free, the region is given back to the kernel, so memory
//    is returned at 2 MiB granularity instead of 256 KiB.
//  - A freed block that is handed out again before its region was given back
//    is cleared first, so every block V8 gets reads as zero, like a fresh
//    mapping does.
//  - Arenas are placed at V8's own random mmap hint, so heap addresses are as
//    random as before, and as repeatable when V8 seeds its generator.
//
// NODE_HEAP_HUGEPAGES_LARGE=1 also asks for huge pages in the 2 MiB-aligned
// part of every other reservation of 2 MiB or more, which is what V8 does when
// built with v8_enable_hugepage (it covers the code range and big objects).
//
// Cage mode. With pointer compression V8 never asks for those 256 KiB pages:
// each isolate reserves one 4 GiB kNoAccess "cage" through this allocator and
// carves its pages out of it itself (base::BoundedPageAllocator, first fit
// from the bottom), calling SetPermissions(kReadWrite) on a chunk to use it
// and SetPermissions(kNoAccess) to free it. A 2 MiB range committed 256 KiB at
// a time by mprotect() and touched right away never gets a huge page. So
// kNoAccess reservations of 1 GiB or more, aligned to 1 GiB or more, are
// recognised as cages and committed in whole 2 MiB regions instead:
//  - The cage is marked MADV_HUGEPAGE once, when it is reserved.
//  - SetPermissions(kReadWrite) on a chunk makes its whole 2 MiB region
//    read-write the first time anything in it is used, so the first touch
//    faults in a huge page; later chunks in the region need no system call.
//    Which 4 KiB pages V8 has made accessible is tracked per region.
//  - SetPermissions(kNoAccess) gives a region back to the kernel (PROT_NONE
//    and MADV_DONTNEED, as V8 does) once nothing in it is in use. Freed
//    chunks in a region that is still in use stay mapped read-write: they
//    do not fault on access until the whole region is freed, like the pages
//    V8 keeps in its own page pool. Once a region is less than half in use,
//    its freed pages go back to the kernel right away instead
//    (NODE_HEAP_HUGEPAGES_SPARSE=<pages> sets the threshold).
//  - Large objects: V8 commits each one's chunk rounded up to 256 KiB but
//    touches only the object, so a huge page would make the rest resident.
//    Node sets V8's --large-objects-at-cage-top, which allocates them from
//    the top of the cage, and commits only the lower half of the cage, where
//    regular pages go, in whole regions; the upper half is left to V8's
//    allocator (NODE_HEAP_HUGEPAGES_LO_TOP=0 manages the whole cage).
//  - WebAssembly memories are large kNoAccess reservations too, but only
//    64 KiB-aligned, and rely on uncommitted pages faulting: never cages.
//  - A freed chunk handed out again before its region was given back is
//    cleared first, so V8 always gets zeroed memory, as it does from a fresh
//    mapping (the V8 cage allows uninitialized pages; cppgc's does not).
//  - Other permissions (read-only space), DecommitPages() (unmapped hole
//    payloads) and DiscardSystemPages() (the sweeper) are applied to the
//    exact range, as V8's allocator does; they split the region's huge page.
//  - The code range (kNoAccessWillJitLater) is left alone, and no mapping is
//    replaced, so the cage keeps the flags V8 gave it (MADV_DONTFORK).
// NODE_HEAP_HUGEPAGES_CAGE=0 turns cage mode off.
//
// NODE_HEAP_HUGEPAGES=count installs the same allocator without arenas: every
// call goes to V8's allocator and is only counted (cages are tracked as if
// cage mode were on, to see how many regions it would commit).
// NODE_HEAP_HUGEPAGES_STATS=<directory> writes the counters to
// <directory>/heap-pages-<pid>.txt every couple of seconds and at exit.

#include "node_heap_page_allocator.h"

#include "node_internals.h"
#include "util.h"

#if defined(__linux__)
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "src/base/page-allocator.h"  // V8's default allocator.
#endif

namespace node {

#if defined(__linux__)
namespace {

constexpr size_t kBlockSize = size_t{256} * 1024;
constexpr size_t kRegionSize = size_t{2} * 1024 * 1024;
constexpr size_t kBlocksPerRegion = kRegionSize / kBlockSize;
static_assert(kBlocksPerRegion == 8, "one byte of block bits per region");
constexpr uint8_t kFullRegion = 0xff;

// Cages are tracked in 4 KiB pages (V8's commit page size on Linux).
constexpr size_t kCagePageSize = size_t{4} * 1024;
constexpr size_t kCagePagesPerRegion = kRegionSize / kCagePageSize;
constexpr size_t kCageWordsPerRegion = kCagePagesPerRegion / 64;
static_assert(kCageWordsPerRegion * 64 == kCagePagesPerRegion, "whole words");
// Smallest size and alignment of a kNoAccess reservation treated as a cage.
// V8's pointer-compression cage is 4 GiB and 4 GiB-aligned; the code range
// (256 MiB, kNoAccessWillJitLater) and WebAssembly memories (64 KiB-aligned)
// are not cages.
constexpr size_t kMinCageSize = size_t{1} << 30;

enum class CageMode { kOff, kCount, kOn };

// Operations on cage memory, for the call-shape table.
enum CageOp {
  kOpReadWrite,
  kOpOtherPermission,
  kOpNoAccess,
  kOpDecommit,
  kOpDiscard,
  kOpRecommit,
  kOpFreePart,
  kOpReleasePart,
  kCageOpCount
};
const char* const kCageOpNames[] = {"rw",
                                    "other_perm",
                                    "noaccess",
                                    "decommit",
                                    "discard",
                                    "recommit",
                                    "free_part",
                                    "release_part"};

// Calls fn(word, mask) for the words of a page bitmap that hold [from, to).
template <typename Fn>
void ForEachMask(size_t from, size_t to, Fn fn) {
  while (from < to) {
    size_t word = from / 64;
    size_t low = from % 64;
    size_t high = std::min<size_t>(64, low + (to - from));
    uint64_t mask = high == 64 ? ~uint64_t{0} : (uint64_t{1} << high) - 1;
    mask &= ~((uint64_t{1} << low) - 1);
    fn(word, mask);
    from += high - low;
  }
}

// Sets the bits [from, to) and returns how many were not set before.
size_t SetBits(uint64_t* words, size_t from, size_t to) {
  size_t count = 0;
  ForEachMask(from, to, [&](size_t word, uint64_t mask) {
    count += __builtin_popcountll(mask & ~words[word]);
    words[word] |= mask;
  });
  return count;
}

// Clears the bits [from, to) and returns how many were set before.
size_t ClearBits(uint64_t* words, size_t from, size_t to) {
  size_t count = 0;
  ForEachMask(from, to, [&](size_t word, uint64_t mask) {
    count += __builtin_popcountll(mask & words[word]);
    words[word] &= ~mask;
  });
  return count;
}

bool AnyBits(const uint64_t* words, size_t from, size_t to) {
  bool any = false;
  ForEachMask(from, to, [&](size_t word, uint64_t mask) {
    if ((words[word] & mask) != 0) any = true;
  });
  return any;
}

bool TestBit(const uint64_t* words, size_t bit) {
  return (words[bit / 64] >> (bit % 64)) & 1;
}

#define HEAP_PAGE_COUNTERS(V)                                                  \
  V(alloc_arena)                                                               \
  V(alloc_pass)                                                                \
  V(alloc_pass_failed)                                                         \
  V(free_arena)                                                                \
  V(free_pass)                                                                 \
  V(perm_rw_noop)                                                              \
  V(perm_arena)                                                                \
  V(perm_pass)                                                                 \
  V(discard_arena)                                                             \
  V(discard_arena_ignored)                                                     \
  V(discard_pass)                                                              \
  V(decommit_arena)                                                            \
  V(decommit_pass)                                                             \
  V(release_arena)                                                             \
  V(release_pass)                                                              \
  V(recommit_arena)                                                            \
  V(recommit_pass)                                                             \
  V(resize_pass)                                                               \
  V(seal_pass)                                                                 \
  V(region_release)                                                            \
  V(region_reset)                                                              \
  V(block_reset)                                                               \
  V(block_cleared)                                                             \
  V(arena_map)                                                                 \
  V(large_advised)                                                             \
  V(arena_map_failed)                                                          \
  V(cage_adopt)                                                                \
  V(cage_free)                                                                 \
  V(cage_rw)                                                                   \
  V(cage_rw_fixup)                                                             \
  V(cage_other_perm)                                                           \
  V(cage_noaccess)                                                             \
  V(cage_noaccess_kept)                                                        \
  V(cage_decommit)                                                             \
  V(cage_discard)                                                              \
  V(cage_discard_ignored)                                                      \
  V(cage_recommit)                                                             \
  V(cage_free_part)                                                            \
  V(cage_release_part)                                                         \
  V(cage_region_commit)                                                        \
  V(cage_region_commit_failed)                                                 \
  V(cage_region_release)                                                       \
  V(cage_region_release_split)                                                 \
  V(cage_pages_cleared)                                                        \
  V(cage_sparse_release)                                                       \
  V(cage_sparse_pages_released)

enum Counter {
#define V(name) k_##name,
  HEAP_PAGE_COUNTERS(V)
#undef V
      kCounterCount
};

const char* const kCounterNames[] = {
#define V(name) #name,
    HEAP_PAGE_COUNTERS(V)
#undef V
};

int ProtectionFor(v8::PageAllocator::Permission permission) {
  switch (permission) {
    case v8::PageAllocator::kNoAccess:
    case v8::PageAllocator::kNoAccessWillJitLater:
      return PROT_NONE;
    case v8::PageAllocator::kRead:
      return PROT_READ;
    case v8::PageAllocator::kReadWrite:
      return PROT_READ | PROT_WRITE;
    case v8::PageAllocator::kReadWriteExecute:
      return PROT_READ | PROT_WRITE | PROT_EXEC;
    case v8::PageAllocator::kReadExecute:
      return PROT_READ | PROT_EXEC;
  }
  UNREACHABLE();
}

// Marks [start, start + size) as wanting huge pages and, like V8's own
// mappings on Linux, as not inherited by fork().
void AdviseRange(uintptr_t start, size_t size) {
  void* address = reinterpret_cast<void*>(start);
  madvise(address, size, MADV_HUGEPAGE);
}

// Replaces [start, start + size) with a fresh read-write mapping. Unlike
// madvise(MADV_DONTNEED), this also frees the page table of a 2 MiB region
// whose huge page was split, so the next touch can fault in a huge page again.
void RemapReadWrite(uintptr_t start, size_t size) {
  void* address = reinterpret_cast<void*>(start);
  void* result = mmap(address,
                      size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1,
                      0);
  CHECK_EQ(result, address);
  AdviseRange(start, size);
}

class HeapPageAllocator final : public v8::PageAllocator {
 public:
  HeapPageAllocator(bool use_arenas,
                    size_t arena_size,
                    bool honor_discard,
                    bool advise_large,
                    CageMode cage_mode,
                    bool large_objects_at_top,
                    size_t sparse_pages,
                    std::string stats_dir)
      : use_arenas_(use_arenas),
        arena_size_(arena_size),
        honor_discard_(honor_discard),
        advise_large_(advise_large),
        cage_mode_(cage_mode),
        large_objects_at_top_(large_objects_at_top),
        sparse_pages_(sparse_pages),
        stats_dir_(std::move(stats_dir)) {}

  size_t AllocatePageSize() override { return default_.AllocatePageSize(); }
  size_t CommitPageSize() override { return default_.CommitPageSize(); }
  void SetRandomMmapSeed(int64_t seed) override {
    default_.SetRandomMmapSeed(seed);
  }
  void* GetRandomMmapAddr() override { return default_.GetRandomMmapAddr(); }

  using v8::PageAllocator::AllocatePages;
  void* AllocatePages(void* hint,
                      size_t length,
                      size_t alignment,
                      Permission permissions) override {
    if (use_arenas_ && length == kBlockSize && permissions == kReadWrite &&
        alignment != 0 && kBlockSize % alignment == 0) {
      void* block = AllocateBlock();
      if (block != nullptr) {
        RecordShape(length, alignment, permissions, true);
        MaybeWriteStats();
        return block;
      }
    }
    RecordShape(length, alignment, permissions, false);
    Count(k_alloc_pass);
    void* result = default_.AllocatePages(hint, length, alignment, permissions);
    if (result == nullptr) {
      Count(k_alloc_pass_failed);
    } else if (cage_mode_ != CageMode::kOff && permissions == kNoAccess &&
               length >= kMinCageSize && alignment >= kMinCageSize &&
               length % kRegionSize == 0 &&
               reinterpret_cast<uintptr_t>(result) % kRegionSize == 0) {
      // A pointer-compression cage (4 GiB, 4 GiB-aligned). WebAssembly
      // memories are large kNoAccess reservations too, but only 64 KiB-aligned;
      // they rely on accesses past the committed size faulting, so they must
      // never be committed in larger steps than V8 asks for.
      AdoptCage(reinterpret_cast<uintptr_t>(result), length);
    } else if (advise_large_ && length >= kRegionSize) {
      // What V8 does itself when built with v8_enable_hugepage: ask for huge
      // pages in the 2 MiB-aligned part of a large reservation. On Linux this
      // covers the code range, which is mapped read-write-execute up front,
      // so JIT code gets huge pages as it fills the range.
      uintptr_t start = RoundUp(reinterpret_cast<uintptr_t>(result), kRegionSize);
      uintptr_t end =
          RoundDown(reinterpret_cast<uintptr_t>(result) + length, kRegionSize);
      if (end > start) {
        madvise(reinterpret_cast<void*>(start), end - start, MADV_HUGEPAGE);
        Count(k_large_advised);
      }
    }
    MaybeWriteStats();
    return result;
  }

  bool FreePages(void* address, size_t length) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (Arena* arena = FindArena(address)) {
        FreeBlock(arena, reinterpret_cast<uintptr_t>(address), length);
        Count(k_free_arena);
        return true;
      }
      uintptr_t start = reinterpret_cast<uintptr_t>(address);
      if (Cage* cage = FindCage(start)) {
        if (start == cage->start &&
            length >= cage->reservation_end - cage->start) {
          DropCage(cage);
        } else {
          // Not expected: V8 frees cage memory with SetPermissions().
          Count(k_cage_free_part);
          RecordCageShape(kOpFreePart, length);
          CageNoAccess(cage, start, length);
        }
      }
    }
    Count(k_free_pass);
    bool result = default_.FreePages(address, length);
    MaybeWriteStats();
    return result;
  }

  bool ReleasePages(void* address, size_t length, size_t new_length) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (Arena* arena = FindArena(address)) {
        // Keep the address range owned by the arena: map the released tail
        // PROT_NONE instead of unmapping it. FreeBlock() restores it.
        uintptr_t start = reinterpret_cast<uintptr_t>(address) + new_length;
        size_t size = length - new_length;
        void* tail = reinterpret_cast<void*>(start);
        void* result =
            mmap(tail,
                 size,
                 PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE,
                 -1,
                 0);
        CHECK_EQ(result, tail);
        MarkChanged(arena, start, size, true);
        Count(k_release_arena);
        return true;
      }
      uintptr_t start = reinterpret_cast<uintptr_t>(address);
      if (Cage* cage = FindCage(start)) {
        // Not expected either (the cage's own allocator releases with
        // SetPermissions()); V8's allocator unmaps the tail.
        Count(k_cage_release_part);
        RecordCageShape(kOpReleasePart, length - new_length);
        CageNoAccess(cage, start + new_length, length - new_length);
      }
    }
    Count(k_release_pass);
    return default_.ReleasePages(address, length, new_length);
  }

  bool SetPermissions(void* address,
                      size_t length,
                      Permission permissions) override {
    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (Arena* arena = FindArena(address)) {
        if (permissions == kReadWrite && !AnyChanged(arena, start, length)) {
          Count(k_perm_rw_noop);
          return true;
        }
        Count(k_perm_arena);
        // Same as V8's OS::SetPermissions() on Linux.
        if (mprotect(address, length, ProtectionFor(permissions)) != 0) {
          CHECK_EQ(errno, ENOMEM);
          return false;
        }
        if (permissions == kNoAccess) {
          CHECK_EQ(madvise(address, length, MADV_DONTNEED), 0);
        }
        MarkChanged(arena, start, length, true);
        return true;
      }
      if (Cage* cage = FindCage(start)) {
        ZeroList zero;
        uintptr_t above = Above(cage, start, length);
        bool result = CageSetPermissions(cage, start, length, permissions, &zero);
        if (cage_mode_ == CageMode::kOn) {
          lock.unlock();
          ClearRanges(zero);
          if (above != 0) {
            result = default_.SetPermissions(reinterpret_cast<void*>(above),
                                             start + length - above,
                                             permissions) &&
                     result;
          }
          MaybeWriteStats();
          return result;
        }
        // Count mode: V8's allocator does the work.
      }
    }
    Count(k_perm_pass);
    bool result = default_.SetPermissions(address, length, permissions);
    if (cage_mode_ == CageMode::kCount) MaybeWriteStats();
    return result;
  }

  bool RecommitPages(void* address,
                     size_t length,
                     Permission permissions) override {
    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (FindArena(address) != nullptr) {
        // V8's allocator does nothing here on Linux either: the pages come
        // back on their next touch.
        Count(k_recommit_arena);
        return true;
      }
      if (Cage* cage = FindCage(start)) {
        // Only the code range is set up to recommit; handled like
        // SetPermissions() in case it ever happens in a cage.
        Count(k_cage_recommit);
        RecordCageShape(kOpRecommit, length);
        ZeroList zero;
        uintptr_t above = Above(cage, start, length);
        bool result = CageSetPermissions(cage, start, length, permissions, &zero);
        if (cage_mode_ == CageMode::kOn) {
          lock.unlock();
          ClearRanges(zero);
          if (above != 0) {
            result = default_.RecommitPages(reinterpret_cast<void*>(above),
                                            start + length - above,
                                            permissions) &&
                     result;
          }
          return result;
        }
      }
    }
    Count(k_recommit_pass);
    return default_.RecommitPages(address, length, permissions);
  }

  bool DiscardSystemPages(void* address, size_t size) override {
    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (Arena* arena = FindArena(address)) {
        if (!honor_discard_) {
          Count(k_discard_arena_ignored);
          return true;
        }
        Count(k_discard_arena);
        CHECK_EQ(madvise(address, size, MADV_DONTNEED), 0);
        // Still read-write, but the huge page of this region is now split.
        MarkChanged(arena, reinterpret_cast<uintptr_t>(address), size, false);
        return true;
      }
      if (Cage* cage = FindCage(start)) {
        RecordCageShape(kOpDiscard, size);
        uintptr_t above = Above(cage, start, size);
        if (!honor_discard_) {
          Count(k_cage_discard_ignored);
          if (cage_mode_ == CageMode::kOn) return true;
        } else {
          Count(k_cage_discard);
          CageDiscard(cage, start, size);
          if (cage_mode_ == CageMode::kOn) {
            return above == 0 ||
                   default_.DiscardSystemPages(reinterpret_cast<void*>(above),
                                               start + size - above);
          }
        }
      }
    }
    Count(k_discard_pass);
    return default_.DiscardSystemPages(address, size);
  }

  bool DecommitPages(void* address, size_t size) override {
    uintptr_t start = reinterpret_cast<uintptr_t>(address);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (Arena* arena = FindArena(address)) {
        // Same as V8's OS::DecommitPages(): inaccessible, and zero when it is
        // made accessible again.
        void* result = mmap(address,
                            size,
                            PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                            -1,
                            0);
        if (result == MAP_FAILED) {
          CHECK_EQ(errno, ENOMEM);
          return false;
        }
        CHECK_EQ(result, address);
        MarkChanged(arena, reinterpret_cast<uintptr_t>(address), size, true);
        Count(k_decommit_arena);
        return true;
      }
      if (Cage* cage = FindCage(start)) {
        Count(k_cage_decommit);
        RecordCageShape(kOpDecommit, size);
        uintptr_t above = Above(cage, start, size);
        bool result = CageDecommit(cage, start, size);
        if (cage_mode_ == CageMode::kOn) {
          return (above == 0 ||
                  default_.DecommitPages(reinterpret_cast<void*>(above),
                                         start + size - above)) &&
                 result;
        }
      }
    }
    Count(k_decommit_pass);
    return default_.DecommitPages(address, size);
  }

  bool ResizeAllocationAt(void* address,
                          size_t old_length,
                          size_t new_length,
                          Permission permissions) override {
    Count(k_resize_pass);
    return default_.ResizeAllocationAt(
        address, old_length, new_length, permissions);
  }

  bool SealPages(void* address, size_t length) override {
    Count(k_seal_pass);
    return default_.SealPages(address, length);
  }

  bool ReserveForSharedMemoryMapping(void* address, size_t size) override {
    return default_.ReserveForSharedMemoryMapping(address, size);
  }

  std::unique_ptr<SharedMemory> AllocateSharedPages(
      size_t length, const void* original_address) override {
    return default_.AllocateSharedPages(length, original_address);
  }

  bool CanAllocateSharedPages() override {
    return default_.CanAllocateSharedPages();
  }

  void WriteStats() {
    if (stats_dir_.empty()) return;
    std::string text;
    char line[256];
    snprintf(line,
             sizeof(line),
             "pid %d mode %s discard %d large %d arena_mib %zu cage %s "
             "lo_top %d sparse %zu\n",
             static_cast<int>(getpid()),
             use_arenas_ ? "arena" : "count",
             honor_discard_ ? 1 : 0,
             advise_large_ ? 1 : 0,
             arena_size_ >> 20,
             cage_mode_ == CageMode::kOn
                 ? "on"
                 : (cage_mode_ == CageMode::kCount ? "count" : "off"),
             large_objects_at_top_ ? 1 : 0,
             sparse_pages_);
    text += line;
    for (int i = 0; i < kCounterCount; i++) {
      snprintf(line,
               sizeof(line),
               "counter %s %" PRIu64 "\n",
               kCounterNames[i],
               counters_[i].load(std::memory_order_relaxed));
      text += line;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      size_t used_regions = 0, changed_regions = 0;
      for (const Arena& arena : arenas_) {
        for (size_t r = 0; r < arena.used.size(); r++) {
          if (arena.used[r] != 0) used_regions++;
          if (arena.split[r] != 0) changed_regions++;
        }
      }
      snprintf(line,
               sizeof(line),
               "arenas %zu live_blocks %zu peak_blocks %zu used_regions %zu "
               "split_regions %zu\n",
               arenas_.size(),
               live_blocks_,
               peak_blocks_,
               used_regions,
               changed_regions);
      text += line;
      size_t split_cage_regions = 0;
      for (const Cage& cage : cages_) {
        size_t regions = (cage.end - cage.start) / kRegionSize;
        for (size_t r = 0; r < regions; r++) {
          if (cage.regions[r].committed && cage.regions[r].split) {
            split_cage_regions++;
          }
        }
      }
      // Regions are 2 MiB, pages 4 KiB.
      snprintf(line,
               sizeof(line),
               "cages %zu committed_regions %zu live_pages %zu "
               "peak_committed_regions %zu live_pages_at_peak %zu "
               "peak_live_pages %zu split_regions %zu\n",
               cages_.size(),
               committed_regions_,
               live_pages_,
               peak_committed_regions_,
               live_pages_at_peak_,
               peak_live_pages_,
               split_cage_regions);
      text += line;
      if (cage_mode_ == CageMode::kCount) {
        std::string residency = ResidencyReport();
        text += residency;
        if (residency_gap_ > max_residency_gap_) {
          max_residency_gap_ = residency_gap_;
          max_gap_report_ = residency;
        }
        // The same lines, from the write where whole regions would have
        // added the most memory over what is resident now.
        size_t start = 0;
        while (start < max_gap_report_.size()) {
          size_t end = max_gap_report_.find('\n', start);
          text += "max_gap_" + max_gap_report_.substr(start, end + 1 - start);
          start = end + 1;
        }
      }
      for (const CageShape& shape : cage_shapes_) {
        snprintf(line,
                 sizeof(line),
                 "cage_shape op %s length %zu count %" PRIu64 "\n",
                 kCageOpNames[shape.op],
                 shape.length,
                 shape.count);
        text += line;
      }
      for (const Shape& shape : shapes_) {
        snprintf(line,
                 sizeof(line),
                 "shape length %zu alignment %zu permission %d arena %d "
                 "count %" PRIu64 " bytes %" PRIu64 "\n",
                 shape.length,
                 shape.alignment,
                 static_cast<int>(shape.permission),
                 shape.arena ? 1 : 0,
                 shape.count,
                 shape.bytes);
        text += line;
      }
    }
    std::string path =
        stats_dir_ + "/heap-pages-" + std::to_string(getpid()) + ".txt";
    std::string temp = path + ".tmp";
    FILE* file = fopen(temp.c_str(), "w");
    if (file == nullptr) return;
    fwrite(text.data(), 1, text.size(), file);
    fclose(file);
    rename(temp.c_str(), path.c_str());
  }

 private:
  struct Arena {
    uintptr_t start;
    uintptr_t end;
    // Per 2 MiB region, one bit per 256 KiB block.
    std::vector<uint8_t> used;     // handed out to V8
    std::vector<uint8_t> changed;  // protection or mapping no longer RW
    std::vector<uint8_t> split;    // the region's huge page may be split
    std::vector<uint8_t> stale;    // freed, still holding V8's old contents
  };

  struct Shape {
    size_t length;
    size_t alignment;
    Permission permission;
    bool arena;
    uint64_t count;
    uint64_t bytes;
  };

  // One 2 MiB region of a cage. Bitmaps have one bit per 4 KiB page.
  struct CageRegion {
    uint64_t live[kCageWordsPerRegion];   // made accessible by V8, not freed
    uint64_t stale[kCageWordsPerRegion];  // freed by V8, region kept: not zero
    uint64_t odd[kCageWordsPerRegion];    // protection is not read-write
    uint16_t live_count;
    bool committed;  // the whole region is mapped (read-write but odd pages)
    bool split;      // part of it was protected or discarded
  };

  struct Cage {
    uintptr_t start;
    uintptr_t end;  // End of the part committed in whole regions.
    CageRegion* regions;
    uintptr_t reservation_end;
  };

  // The part of [start, start + length) above the managed part of |cage|,
  // which is passed to V8's allocator.
  static uintptr_t Above(const Cage* cage, uintptr_t start, size_t length) {
    return start + length > cage->end ? cage->end : 0;
  }

  struct ZeroRange {
    uintptr_t start;
    size_t size;
  };
  using ZeroList = std::vector<ZeroRange>;

  struct CageShape {
    CageOp op;
    size_t length;
    uint64_t count;
  };

  // Whether cage calls are carried out here (on) or only tracked (count).
  bool ApplyCage() const { return cage_mode_ == CageMode::kOn; }

  void AdoptCage(uintptr_t start, size_t length) {
    // calloc() maps large blocks fresh, so the bookkeeping of regions V8
    // never uses is never touched.
    // With large objects at the top of the cage (V8's
    // --large-objects-at-cage-top), only the lower half, where regular pages
    // go, is committed in whole regions; the upper half is left to V8's
    // allocator, so large objects do not pull the untouched rest of their
    // chunks onto huge pages.
    size_t managed = large_objects_at_top_ ? length / 2 : length;
    CageRegion* regions = static_cast<CageRegion*>(
        calloc(managed / kRegionSize, sizeof(CageRegion)));
    if (regions == nullptr) return;  // Stays an ordinary reservation.
    if (ApplyCage()) {
      // One call for the whole managed part; the read-write ranges carved out
      // of it later keep the flag.
      madvise(reinterpret_cast<void*>(start), managed, MADV_HUGEPAGE);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    cages_.push_back({start, start + managed, regions, start + length});
    Count(k_cage_adopt);
  }

  // Requires mutex_.
  Cage* FindCage(uintptr_t address) {
    for (Cage& cage : cages_) {
      if (address >= cage.start && address < cage.end) return &cage;
    }
    return nullptr;
  }

  // Requires mutex_. The whole reservation is being freed.
  void DropCage(Cage* cage) {
    size_t regions = (cage->end - cage->start) / kRegionSize;
    for (size_t r = 0; r < regions; r++) {
      if (cage->regions[r].committed) {
        committed_regions_--;
        live_pages_ -= cage->regions[r].live_count;
      }
    }
    free(cage->regions);
    chunks_.erase(chunks_.lower_bound(cage->start),
                  chunks_.lower_bound(cage->end));
    cages_.erase(cages_.begin() + (cage - cages_.data()));
    Count(k_cage_free);
  }

  // Requires mutex_. Calls fn(region, region_start, from, to) for each 2 MiB
  // region overlapping [start, start + length), with [from, to) the pages of
  // the region in the range.
  template <typename Fn>
  void ForEachCageRegion(Cage* cage, uintptr_t start, size_t length, Fn fn) {
    uintptr_t end = std::min(start + length, cage->end);
    for (uintptr_t base = RoundDown(start, kRegionSize); base < end;
         base += kRegionSize) {
      size_t from = (std::max(start, base) - base) / kCagePageSize;
      size_t to = (std::min(end, base + kRegionSize) - base + kCagePageSize - 1) /
                  kCagePageSize;
      fn(cage->regions[(base - cage->start) / kRegionSize], base, from, to);
    }
  }

  static void* PageAddress(uintptr_t base, size_t page) {
    return reinterpret_cast<void*>(base + page * kCagePageSize);
  }

  // Requires mutex_. Makes the whole region read-write; its first touch then
  // faults in a huge page.
  bool CommitRegion(CageRegion& region, uintptr_t base) {
    if (ApplyCage() && mprotect(reinterpret_cast<void*>(base),
                                kRegionSize,
                                PROT_READ | PROT_WRITE) != 0) {
      CHECK_EQ(errno, ENOMEM);
      Count(k_cage_region_commit_failed);
      return false;
    }
    region.committed = true;
    committed_regions_++;
    Count(k_cage_region_commit);
    return true;
  }

  // Requires mutex_. Nothing in the region is in use: give it back, as V8's
  // SetPermissions(kNoAccess) does. With CONFIG_PT_RECLAIM, MADV_DONTNEED of
  // the whole region also frees the page table of a split region, so it can
  // get a huge page again.
  void ReleaseRegion(CageRegion& region, uintptr_t base) {
    if (ApplyCage()) {
      void* address = reinterpret_cast<void*>(base);
      if (mprotect(address, kRegionSize, PROT_NONE) != 0) {
        CHECK_EQ(errno, ENOMEM);
      }
      madvise(address, kRegionSize, MADV_DONTNEED);
    }
    Count(region.split ? k_cage_region_release_split : k_cage_region_release);
    memset(&region, 0, sizeof(region));
    committed_regions_--;
  }

  // Requires mutex_.
  void AddLive(CageRegion& region, size_t from, size_t to) {
    size_t added = SetBits(region.live, from, to);
    region.live_count += added;
    live_pages_ += added;
  }

  // Requires mutex_.
  void RemoveLive(CageRegion& region, size_t from, size_t to) {
    size_t removed = ClearBits(region.live, from, to);
    region.live_count -= removed;
    live_pages_ -= removed;
  }

  // Requires mutex_.
  void UpdatePeaks() {
    if (committed_regions_ > peak_committed_regions_) {
      peak_committed_regions_ = committed_regions_;
      live_pages_at_peak_ = live_pages_;
    }
    if (live_pages_ > peak_live_pages_) peak_live_pages_ = live_pages_;
  }

  // Requires mutex_. Clears the stale pages in [from, to): into |zero| to be
  // cleared after the lock is dropped, or right away when |zero| is null.
  // Stale pages are always read-write.
  void ClearStale(CageRegion& region,
                  uintptr_t base,
                  size_t from,
                  size_t to,
                  ZeroList* zero) {
    if (!AnyBits(region.stale, from, to)) return;
    for (size_t page = from; page < to;) {
      if (!TestBit(region.stale, page)) {
        page++;
        continue;
      }
      size_t end = page;
      while (end < to && TestBit(region.stale, end)) end++;
      if (ApplyCage()) {
        ZeroRange range{reinterpret_cast<uintptr_t>(PageAddress(base, page)),
                        (end - page) * kCagePageSize};
        if (zero != nullptr) {
          zero->push_back(range);
        } else {
          memset(reinterpret_cast<void*>(range.start), 0, range.size);
        }
      }
      counters_[k_cage_pages_cleared].fetch_add(end - page,
                                                std::memory_order_relaxed);
      page = end;
    }
    ClearBits(region.stale, from, to);
  }

  void ClearRanges(const ZeroList& zero) {
    for (const ZeroRange& range : zero) {
      memset(reinterpret_cast<void*>(range.start), 0, range.size);
    }
  }

  // Requires mutex_.
  bool CageSetPermissions(Cage* cage,
                          uintptr_t start,
                          size_t length,
                          Permission permission,
                          ZeroList* zero) {
    if (permission == kNoAccess || permission == kNoAccessWillJitLater) {
      Count(k_cage_noaccess);
      RecordCageShape(kOpNoAccess, length);
      CageNoAccess(cage, start, length);
      return true;
    }
    bool read_write = permission == kReadWrite;
    Count(read_write ? k_cage_rw : k_cage_other_perm);
    RecordCageShape(read_write ? kOpReadWrite : kOpOtherPermission, length);
    // Commit every region first, so that running out of memory leaves
    // nothing half done.
    bool ok = true;
    ForEachCageRegion(
        cage, start, length, [&](CageRegion& region, uintptr_t base, size_t, size_t) {
          if (ok && !region.committed && !CommitRegion(region, base)) ok = false;
        });
    if (!ok) {
      ForEachCageRegion(
          cage, start, length, [&](CageRegion& region, uintptr_t base, size_t, size_t) {
            if (region.committed && region.live_count == 0) {
              ReleaseRegion(region, base);
            }
          });
      return false;
    }
    ForEachCageRegion(
        cage,
        start,
        length,
        [&](CageRegion& region, uintptr_t base, size_t from, size_t to) {
          if (!ok) return;
          void* address = PageAddress(base, from);
          size_t size = (to - from) * kCagePageSize;
          // Old contents must go before the pages can lose write access.
          ClearStale(region, base, from, to, read_write ? zero : nullptr);
          if (read_write) {
            if (AnyBits(region.odd, from, to)) {
              if (ApplyCage() &&
                  mprotect(address, size, PROT_READ | PROT_WRITE) != 0) {
                CHECK_EQ(errno, ENOMEM);
                ok = false;
                return;
              }
              ClearBits(region.odd, from, to);
              Count(k_cage_rw_fixup);
            }
          } else {
            if (ApplyCage() &&
                mprotect(address, size, ProtectionFor(permission)) != 0) {
              CHECK_EQ(errno, ENOMEM);
              ok = false;
              return;
            }
            SetBits(region.odd, from, to);
            region.split = true;
          }
          AddLive(region, from, to);
        });
    if (TrackingChunks()) TrackChunk(start, length);
    UpdatePeaks();
    return ok;
  }

  // Requires mutex_. V8 frees [start, start + length).
  void CageNoAccess(Cage* cage, uintptr_t start, size_t length) {
    if (TrackingChunks()) UntrackChunk(start, length);
    ForEachCageRegion(
        cage,
        start,
        length,
        [&](CageRegion& region, uintptr_t base, size_t from, size_t to) {
          if (!region.committed) return;  // Already inaccessible.
          RemoveLive(region, from, to);
          if (region.live_count == 0) {
            ReleaseRegion(region, base);
            return;
          }
          // The rest of the region is in use: keep the freed pages mapped
          // read-write, to be cleared if they are used again.
          Count(k_cage_noaccess_kept);
          if (AnyBits(region.odd, from, to)) {
            if (!ApplyCage() || mprotect(PageAddress(base, from),
                                         (to - from) * kCagePageSize,
                                         PROT_READ | PROT_WRITE) == 0) {
              ClearBits(region.odd, from, to);
            } else {
              CHECK_EQ(errno, ENOMEM);
              return;  // Left as it is; not marked stale, so never written.
            }
          }
          SetBits(region.stale, from, to);
          if (region.live_count < sparse_pages_) GiveBackStale(region, base);
        });
  }

  // Requires mutex_. The region is mostly free: give the freed pages back to
  // the kernel now, as V8's allocator would have, instead of keeping them
  // until the whole region is free. This splits the region's huge page.
  void GiveBackStale(CageRegion& region, uintptr_t base) {
    for (size_t page = 0; page < kCagePagesPerRegion;) {
      if (!TestBit(region.stale, page)) {
        page++;
        continue;
      }
      size_t end = page;
      while (end < kCagePagesPerRegion && TestBit(region.stale, end)) end++;
      if (ApplyCage()) {
        CHECK_EQ(madvise(PageAddress(base, page),
                         (end - page) * kCagePageSize,
                         MADV_DONTNEED),
                 0);
      }
      counters_[k_cage_sparse_pages_released].fetch_add(
          end - page, std::memory_order_relaxed);
      page = end;
    }
    memset(region.stale, 0, sizeof(region.stale));
    region.split = true;
    Count(k_cage_sparse_release);
  }

  // Requires mutex_. As V8's DecommitPages(): inaccessible, zero when made
  // accessible again. Applied to the exact range (it splits the huge page).
  bool CageDecommit(Cage* cage, uintptr_t start, size_t length) {
    bool ok = true;
    ForEachCageRegion(
        cage,
        start,
        length,
        [&](CageRegion& region, uintptr_t base, size_t from, size_t to) {
          if (!ok || !region.committed) return;  // Uncommitted: already so.
          void* address = PageAddress(base, from);
          size_t size = (to - from) * kCagePageSize;
          if (ApplyCage()) {
            if (mprotect(address, size, PROT_NONE) != 0) {
              CHECK_EQ(errno, ENOMEM);
              ok = false;
              return;
            }
            CHECK_EQ(madvise(address, size, MADV_DONTNEED), 0);
          }
          SetBits(region.odd, from, to);
          ClearBits(region.stale, from, to);
          region.split = true;
          RemoveLive(region, from, to);
          if (region.live_count == 0) ReleaseRegion(region, base);
        });
    return ok;
  }

  // Requires mutex_. A hint to drop the contents; the pages stay accessible.
  void CageDiscard(Cage* cage, uintptr_t start, size_t length) {
    ForEachCageRegion(
        cage,
        start,
        length,
        [&](CageRegion& region, uintptr_t base, size_t from, size_t to) {
          if (!region.committed) return;
          if (ApplyCage()) {
            CHECK_EQ(madvise(PageAddress(base, from),
                             (to - from) * kCagePageSize,
                             MADV_DONTNEED),
                     0);
          }
          ClearBits(region.stale, from, to);
          region.split = true;
        });
  }

  // Count mode with stats only: the chunks V8 has made accessible, to see how
  // much of each is resident when V8's allocator commits it.
  bool TrackingChunks() const {
    return cage_mode_ == CageMode::kCount && !stats_dir_.empty();
  }

  // Requires mutex_.
  void TrackChunk(uintptr_t start, size_t length) {
    if (chunks_.count(start) == 0) chunks_[start] = {length, length};
  }

  // Requires mutex_. V8 frees a whole chunk, or the tail of a large object.
  void UntrackChunk(uintptr_t start, size_t length) {
    auto it = chunks_.upper_bound(start);
    if (it == chunks_.begin()) return;
    --it;
    if (it->first == start) {
      chunks_.erase(it);
    } else if (start < it->first + it->second.length) {
      it->second.length = start - it->first;
    }
  }

  // Requires mutex_. With V8's own allocator (count mode): resident pages of
  // the accessible chunks, by kind, and how many pages whole 2 MiB regions
  // would add over that.
  std::string ResidencyReport() {
    struct Kind {
      const char* name;
      size_t chunks, pages, resident;
    };
    Kind kinds[] = {{"page", 0, 0, 0},      // one 256 KiB chunk
                    {"large", 0, 0, 0},     // larger chunk (large object)
                    {"shrunk", 0, 0, 0},    // large object whose tail was freed
                    {"other", 0, 0, 0}};
    size_t histogram[5] = {};  // resident share of "page" chunks, by quarter
    std::vector<unsigned char> vector;
    size_t resident_total = 0;
    for (const auto& entry : chunks_) {
      const TrackedChunk& chunk = entry.second;
      size_t pages = chunk.length / kCagePageSize;
      if (pages == 0) continue;
      vector.resize(pages);
      if (mincore(reinterpret_cast<void*>(entry.first),
                  chunk.length,
                  vector.data()) != 0) {
        continue;
      }
      size_t resident = 0;
      for (unsigned char v : vector) resident += v & 1;
      int kind = chunk.length != chunk.original ? 2
                 : chunk.original == kBlockSize ? 0
                 : chunk.original > kBlockSize  ? 1
                                                : 3;
      kinds[kind].chunks++;
      kinds[kind].pages += pages;
      kinds[kind].resident += resident;
      resident_total += resident;
      if (kind == 0) histogram[resident == pages ? 4 : resident * 4 / pages]++;
    }
    size_t committed_pages = committed_regions_ * kCagePagesPerRegion;
    residency_gap_ =
        committed_pages > resident_total ? committed_pages - resident_total : 0;
    std::string text;
    char line[256];
    snprintf(line,
             sizeof(line),
             "residency region_pages %zu live_pages %zu resident_pages %zu\n",
             committed_pages,
             live_pages_,
             resident_total);
    text += line;
    for (const Kind& kind : kinds) {
      snprintf(line,
               sizeof(line),
               "residency_kind %s chunks %zu pages %zu resident %zu\n",
               kind.name,
               kind.chunks,
               kind.pages,
               kind.resident);
      text += line;
    }
    snprintf(line,
             sizeof(line),
             "residency_page_quarters %zu %zu %zu %zu full %zu\n",
             histogram[0],
             histogram[1],
             histogram[2],
             histogram[3],
             histogram[4]);
    text += line;
    return text;
  }

  // Requires mutex_.
  void RecordCageShape(CageOp op, size_t length) {
    if (stats_dir_.empty()) return;
    for (CageShape& shape : cage_shapes_) {
      if (shape.op == op && shape.length == length) {
        shape.count++;
        return;
      }
    }
    if (cage_shapes_.size() < 256) cage_shapes_.push_back({op, length, 1});
  }

  void Count(Counter counter) {
    counters_[counter].fetch_add(1, std::memory_order_relaxed);
  }

  void RecordShape(size_t length,
                   size_t alignment,
                   Permission permission,
                   bool arena) {
    if (stats_dir_.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    for (Shape& shape : shapes_) {
      if (shape.length == length && shape.alignment == alignment &&
          shape.permission == permission && shape.arena == arena) {
        shape.count++;
        shape.bytes += length;
        return;
      }
    }
    if (shapes_.size() < 256) {
      shapes_.push_back({length, alignment, permission, arena, 1, length});
    }
  }

  void MaybeWriteStats() {
    if (stats_dir_.empty()) return;
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    int64_t last = last_stats_write_.load(std::memory_order_relaxed);
    if (now.tv_sec - last < 2) return;
    if (!last_stats_write_.compare_exchange_strong(last, now.tv_sec)) return;
    WriteStats();
  }

  // Requires mutex_.
  Arena* FindArena(const void* address) {
    uintptr_t value = reinterpret_cast<uintptr_t>(address);
    for (Arena& arena : arenas_) {
      if (value >= arena.start && value < arena.end) return &arena;
    }
    return nullptr;
  }

  // Requires mutex_.
  bool MapArena() {
    size_t size = arena_size_;
    // Place arenas where V8 would place its own mappings: at a random
    // address from V8's generator, which is repeatable when V8 seeds it
    // (--random-seed), so heap addresses stay reproducible too.
    void* hint = reinterpret_cast<void*>(RoundUp(
        reinterpret_cast<uintptr_t>(default_.GetRandomMmapAddr()), kRegionSize));
    void* raw = mmap(hint,
                     size + kRegionSize,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS,
                     -1,
                     0);
    if (raw == MAP_FAILED) {
      Count(k_arena_map_failed);
      return false;
    }
    uintptr_t raw_start = reinterpret_cast<uintptr_t>(raw);
    uintptr_t start = RoundUp(raw_start, kRegionSize);
    if (start != raw_start) {
      munmap(raw, start - raw_start);
    }
    uintptr_t end = start + size;
    uintptr_t raw_end = raw_start + size + kRegionSize;
    if (raw_end != end) {
      munmap(reinterpret_cast<void*>(end), raw_end - end);
    }
    AdviseRange(start, size);
    size_t regions = size / kRegionSize;
    arenas_.push_back({start,
                       end,
                       std::vector<uint8_t>(regions, 0),
                       std::vector<uint8_t>(regions, 0),
                       std::vector<uint8_t>(regions, 0),
                       std::vector<uint8_t>(regions, 0)});
    Count(k_arena_map);
    return true;
  }

  void* AllocateBlock() {
    bool stale = false;
    void* block = AllocateBlockLocked(&stale);
    if (stale) {
      // The block is ours alone now; clear it outside the lock.
      memset(block, 0, kBlockSize);
      Count(k_block_cleared);
    }
    return block;
  }

  void* AllocateBlockLocked(bool* stale) {
    std::lock_guard<std::mutex> lock(mutex_);
    Arena* found = nullptr;
    size_t region = 0;
    // A region that is partly in use already has its huge page; fill it
    // first. Otherwise take the lowest empty region.
    for (int pass = 0; pass < 2 && found == nullptr; pass++) {
      for (Arena& arena : arenas_) {
        for (size_t r = 0; r < arena.used.size(); r++) {
          uint8_t used = arena.used[r];
          bool match = pass == 0 ? (used != 0 && used != kFullRegion)
                                 : (used == 0);
          if (match) {
            found = &arena;
            region = r;
            break;
          }
        }
        if (found != nullptr) break;
      }
    }
    if (found == nullptr) {
      if (!MapArena()) return nullptr;
      found = &arenas_.back();
      region = 0;
    }
    uint8_t free_bits = static_cast<uint8_t>(~found->used[region]);
    int bit = __builtin_ctz(free_bits);
    uint8_t mask = static_cast<uint8_t>(1u << bit);
    found->used[region] |= mask;
    if ((found->stale[region] & mask) != 0) {
      found->stale[region] &= static_cast<uint8_t>(~mask);
      *stale = true;
    }
    live_blocks_++;
    if (live_blocks_ > peak_blocks_) peak_blocks_ = live_blocks_;
    Count(k_alloc_arena);
    return reinterpret_cast<void*>(found->start + region * kRegionSize +
                                   bit * kBlockSize);
  }

  // Requires mutex_.
  void FreeBlock(Arena* arena, uintptr_t address, size_t length) {
    CHECK_EQ(address % kBlockSize, 0);
    CHECK_LE(length, kBlockSize);
    size_t offset = address - arena->start;
    size_t region = offset / kRegionSize;
    uint8_t bit = static_cast<uint8_t>(1u << ((offset % kRegionSize) / kBlockSize));
    CHECK_NE(arena->used[region] & bit, 0);
    arena->used[region] &= static_cast<uint8_t>(~bit);
    live_blocks_--;
    uintptr_t region_start = arena->start + region * kRegionSize;
    if (arena->used[region] == 0) {
      // The whole region is free: give its memory back to the kernel.
      if (arena->split[region] != 0 || arena->changed[region] != 0) {
        RemapReadWrite(region_start, kRegionSize);
        Count(k_region_reset);
      } else {
        CHECK_EQ(madvise(reinterpret_cast<void*>(region_start),
                         kRegionSize,
                         MADV_DONTNEED),
                 0);
        Count(k_region_release);
      }
      arena->split[region] = 0;
      arena->changed[region] = 0;
      arena->stale[region] = 0;
    } else if ((arena->changed[region] & bit) != 0) {
      // Make the block plain read-write (and zero) again before it is handed
      // out again.
      RemapReadWrite(address, kBlockSize);
      arena->changed[region] &= static_cast<uint8_t>(~bit);
      Count(k_block_reset);
    } else {
      // Cleared lazily, if it is handed out again before its region is freed.
      arena->stale[region] |= bit;
    }
  }

  // Requires mutex_. Records that [start, start + size) was changed: always
  // splitting its regions' huge pages, and with |protection| also leaving the
  // blocks something other than plain read-write.
  void MarkChanged(Arena* arena, uintptr_t start, size_t size, bool protection) {
    uintptr_t end = start + size;
    for (uintptr_t block = RoundDown(start, kBlockSize); block < end;
         block += kBlockSize) {
      size_t offset = block - arena->start;
      size_t region = offset / kRegionSize;
      arena->split[region] = 1;
      if (protection) {
        arena->changed[region] |=
            static_cast<uint8_t>(1u << ((offset % kRegionSize) / kBlockSize));
      }
    }
  }

  // Requires mutex_.
  bool AnyChanged(Arena* arena, uintptr_t start, size_t size) {
    uintptr_t end = start + size;
    for (uintptr_t block = RoundDown(start, kBlockSize); block < end;
         block += kBlockSize) {
      size_t offset = block - arena->start;
      size_t region = offset / kRegionSize;
      uint8_t bit = static_cast<uint8_t>(1u << ((offset % kRegionSize) / kBlockSize));
      if ((arena->changed[region] & bit) != 0) return true;
    }
    return false;
  }

  static uintptr_t RoundUp(uintptr_t value, size_t alignment) {
    return (value + alignment - 1) & ~(static_cast<uintptr_t>(alignment) - 1);
  }
  static uintptr_t RoundDown(uintptr_t value, size_t alignment) {
    return value & ~(static_cast<uintptr_t>(alignment) - 1);
  }

  v8::base::PageAllocator default_;
  const bool use_arenas_;
  const size_t arena_size_;
  const bool honor_discard_;
  const bool advise_large_;
  const CageMode cage_mode_;
  const bool large_objects_at_top_;
  // A region with fewer live pages than this gives freed pages back at once.
  const size_t sparse_pages_;
  const std::string stats_dir_;

  std::mutex mutex_;
  std::vector<Cage> cages_;
  size_t committed_regions_ = 0;
  size_t live_pages_ = 0;
  size_t peak_committed_regions_ = 0;
  size_t live_pages_at_peak_ = 0;
  size_t peak_live_pages_ = 0;
  std::vector<CageShape> cage_shapes_;
  struct TrackedChunk {
    size_t length;
    size_t original;
  };
  std::map<uintptr_t, TrackedChunk> chunks_;  // count mode with stats only
  size_t residency_gap_ = 0;
  size_t max_residency_gap_ = 0;
  std::string max_gap_report_;
  std::vector<Arena> arenas_;
  size_t live_blocks_ = 0;
  size_t peak_blocks_ = 0;
  std::vector<Shape> shapes_;
  std::atomic<uint64_t> counters_[kCounterCount] = {};
  std::atomic<int64_t> last_stats_write_{0};
};

HeapPageAllocator* heap_page_allocator = nullptr;

void WriteStatsAtExit() {
  if (heap_page_allocator != nullptr) heap_page_allocator->WriteStats();
}

HeapPageAllocator* CreateFromEnvironment() {
  std::string mode;
  if (!credentials::SafeGetenv("NODE_HEAP_HUGEPAGES", &mode) || mode.empty() ||
      mode == "0") {
    return nullptr;
  }
  bool use_arenas;
  if (mode == "1") {
    use_arenas = true;
  } else if (mode == "count") {
    use_arenas = false;
  } else {
    fprintf(stderr,
            "node: ignoring NODE_HEAP_HUGEPAGES=%s (expected 0, 1 or count)\n",
            mode.c_str());
    return nullptr;
  }
  size_t arena_size = size_t{128} << 20;
  std::string value;
  if (credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_ARENA_MB", &value)) {
    uint64_t mib = strtoull(value.c_str(), nullptr, 10);
    if (mib >= 2) arena_size = static_cast<size_t>(mib & ~uint64_t{1}) << 20;
  }
  bool honor_discard = true;
  if (credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_DISCARD", &value) &&
      value == "0") {
    honor_discard = false;
  }
  bool advise_large = credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_LARGE",
                                              &value) &&
                      value == "1";
  CageMode cage_mode = use_arenas ? CageMode::kOn : CageMode::kCount;
  if ((credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_CAGE", &value) &&
       value == "0") ||
      sysconf(_SC_PAGESIZE) != static_cast<long>(kCagePageSize)) {
    // 2 MiB is the huge page size only with 4 KiB base pages.
    cage_mode = CageMode::kOff;
  }
  // Large objects go to the top half of each cage, which is left on small
  // pages (on by default with cage mode, NODE_HEAP_HUGEPAGES_LO_TOP=0 turns it
  // off; =1 also turns it on in count mode).
  bool large_objects_at_top = cage_mode == CageMode::kOn;
  if (credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_LO_TOP", &value)) {
    large_objects_at_top = cage_mode != CageMode::kOff && value == "1";
  }
  if (large_objects_at_top) {
    v8::V8::SetFlagsFromString("--large-objects-at-cage-top");
  }
  // Freed pages in a region that is less than half in use go back to the
  // kernel right away (NODE_HEAP_HUGEPAGES_SPARSE=<pages>, 0 keeps them until
  // the whole region is free).
  size_t sparse_pages = kCagePagesPerRegion / 2;
  if (credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_SPARSE", &value)) {
    sparse_pages = std::min<size_t>(strtoull(value.c_str(), nullptr, 10),
                                    kCagePagesPerRegion);
  }
  std::string stats_dir;
  credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_STATS", &stats_dir);
  HeapPageAllocator* allocator = new HeapPageAllocator(use_arenas,
                                                       arena_size,
                                                       honor_discard,
                                                       advise_large,
                                                       cage_mode,
                                                       large_objects_at_top,
                                                       sparse_pages,
                                                       std::move(stats_dir));
  heap_page_allocator = allocator;
  atexit(WriteStatsAtExit);
  return allocator;
}

// Executable text is mapped from the binary in 4 KiB pages (file-backed text
// can only get huge pages with CONFIG_READ_ONLY_THP_FOR_FS). This copies the
// chosen 2 MiB-aligned window into an anonymous MADV_HUGEPAGE mapping and moves
// that mapping over the original with mremap(), which swaps it atomically.
// The bytes are identical, so threads running in the window are unaffected.
void RemapText(v8::Isolate* isolate) {
  std::string mode;
  if (!credentials::SafeGetenv("NODE_TEXT_HUGEPAGES", &mode) || mode.empty() ||
      mode == "0") {
    return;
  }
  if (mode != "builtins" && mode != "all") {
    fprintf(stderr,
            "node: ignoring NODE_TEXT_HUGEPAGES=%s (expected builtins or all)\n",
            mode.c_str());
    return;
  }
  const void* code = nullptr;
  size_t code_size = 0;
  isolate->GetEmbeddedCodeRange(&code, &code_size);
  uintptr_t code_start = reinterpret_cast<uintptr_t>(code);
  uintptr_t code_end = code_start + code_size;
  if (code_start == 0) return;

  // The executable mapping that holds the builtins.
  uintptr_t text_start = 0, text_end = 0;
  FILE* maps = fopen("/proc/self/maps", "r");
  if (maps == nullptr) return;
  char line[512];
  while (fgets(line, sizeof(line), maps) != nullptr) {
    uintptr_t lo, hi;
    char perms[5];
    if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %4s", &lo, &hi, perms) != 3) {
      continue;
    }
    if (perms[2] == 'x' && lo <= code_start && code_start < hi) {
      text_start = lo;
      text_end = hi;
      break;
    }
  }
  fclose(maps);
  if (text_start == 0) return;

  auto round_up = [](uintptr_t value) {
    return (value + kRegionSize - 1) & ~(kRegionSize - 1);
  };
  auto round_down = [](uintptr_t value) { return value & ~(kRegionSize - 1); };
  uintptr_t start = round_up(text_start);
  uintptr_t end = round_down(text_end);
  if (mode == "builtins") {
    start = std::max(start, round_down(code_start));
    end = std::min(end, round_up(code_end));
  }
  if (end <= start) return;
  size_t size = end - start;

  void* raw = mmap(nullptr,
                   size + kRegionSize,
                   PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS,
                   -1,
                   0);
  if (raw == MAP_FAILED) return;
  uintptr_t raw_start = reinterpret_cast<uintptr_t>(raw);
  uintptr_t copy = round_up(raw_start);
  if (copy != raw_start) munmap(raw, copy - raw_start);
  uintptr_t raw_end = raw_start + size + kRegionSize;
  if (raw_end != copy + size) {
    munmap(reinterpret_cast<void*>(copy + size), raw_end - (copy + size));
  }
  void* copy_address = reinterpret_cast<void*>(copy);
  madvise(copy_address, size, MADV_HUGEPAGE);
  memcpy(copy_address, reinterpret_cast<void*>(start), size);
  __builtin___clear_cache(static_cast<char*>(copy_address),
                          static_cast<char*>(copy_address) + size);
  if (mprotect(copy_address, size, PROT_READ | PROT_EXEC) != 0 ||
      mremap(copy_address,
             size,
             size,
             MREMAP_MAYMOVE | MREMAP_FIXED,
             reinterpret_cast<void*>(start)) == MAP_FAILED) {
    fprintf(stderr,
            "node: NODE_TEXT_HUGEPAGES: could not remap text: %s\n",
            strerror(errno));
    munmap(copy_address, size);
    return;
  }
  std::string stats_dir;
  if (credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_STATS", &stats_dir) &&
      !stats_dir.empty()) {
    std::string path =
        stats_dir + "/text-" + std::to_string(getpid()) + ".txt";
    FILE* file = fopen(path.c_str(), "w");
    if (file != nullptr) {
      fprintf(file,
              "mode %s text %zu MiB builtins %zu KiB remapped %zu MiB\n",
              mode.c_str(),
              static_cast<size_t>(text_end - text_start) >> 20,
              code_size >> 10,
              size >> 20);
      fclose(file);
    }
  }
}

}  // namespace
#endif  // defined(__linux__)

void MaybeRemapTextToHugePages(v8::Isolate* isolate) {
#if defined(__linux__)
  static std::once_flag once;
  std::call_once(once, [isolate]() { RemapText(isolate); });
#endif
}

v8::PageAllocator* GetHeapPageAllocatorFromEnvironment() {
#if defined(__linux__)
  // Never freed: V8 may use it until the process exits.
  static v8::PageAllocator* allocator = CreateFromEnvironment();
  return allocator;
#else
  return nullptr;
#endif
}

}  // namespace node
