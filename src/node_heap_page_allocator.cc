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
// NODE_HEAP_HUGEPAGES=count installs the same allocator without arenas: every
// call goes to V8's allocator and is only counted. NODE_HEAP_HUGEPAGES_STATS=
// <directory> writes the counters to <directory>/heap-pages-<pid>.txt every
// couple of seconds and at exit.

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
  V(arena_map_failed)

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
  madvise(address, size, MADV_DONTFORK);
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
                    std::string stats_dir)
      : use_arenas_(use_arenas),
        arena_size_(arena_size),
        honor_discard_(honor_discard),
        advise_large_(advise_large),
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
    }
    Count(k_release_pass);
    return default_.ReleasePages(address, length, new_length);
  }

  bool SetPermissions(void* address,
                      size_t length,
                      Permission permissions) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (Arena* arena = FindArena(address)) {
        uintptr_t start = reinterpret_cast<uintptr_t>(address);
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
    }
    Count(k_perm_pass);
    return default_.SetPermissions(address, length, permissions);
  }

  bool RecommitPages(void* address,
                     size_t length,
                     Permission permissions) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (FindArena(address) != nullptr) {
        // V8's allocator does nothing here on Linux either: the pages come
        // back on their next touch.
        Count(k_recommit_arena);
        return true;
      }
    }
    Count(k_recommit_pass);
    return default_.RecommitPages(address, length, permissions);
  }

  bool DiscardSystemPages(void* address, size_t size) override {
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
    }
    Count(k_discard_pass);
    return default_.DiscardSystemPages(address, size);
  }

  bool DecommitPages(void* address, size_t size) override {
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
             "pid %d mode %s discard %d large %d arena_mib %zu\n",
             static_cast<int>(getpid()),
             use_arenas_ ? "arena" : "count",
             honor_discard_ ? 1 : 0,
             advise_large_ ? 1 : 0,
             arena_size_ >> 20);
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
  const std::string stats_dir_;

  std::mutex mutex_;
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
  std::string stats_dir;
  credentials::SafeGetenv("NODE_HEAP_HUGEPAGES_STATS", &stats_dir);
  HeapPageAllocator* allocator = new HeapPageAllocator(use_arenas,
                                                       arena_size,
                                                       honor_discard,
                                                       advise_large,
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
