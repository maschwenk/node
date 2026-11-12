#ifndef SRC_COMPILE_CACHE_H_
#define SRC_COMPILE_CACHE_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include <cinttypes>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include "v8.h"

namespace node {
class Environment;

#define CACHED_CODE_TYPES(V)                                                   \
  V(kCommonJS, 0)                                                              \
  V(kESM, 1)                                                                   \
  V(kStrippedTypeScript, 2)                                                    \
  V(kTransformedTypeScript, 3)                                                 \
  V(kTransformedTypeScriptWithSourceMaps, 4)

enum class CachedCodeType : uint8_t {
#define V(type, value) type = value,
  CACHED_CODE_TYPES(V)
#undef V
};

struct CompileCacheEntry {
  std::unique_ptr<v8::ScriptCompiler::CachedData> cache{nullptr};
  uint32_t cache_key;
  uint32_t code_hash;
  uint32_t code_size;

  std::string cache_filename;
  std::string source_filename;
  CachedCodeType type;
  bool refreshed = false;
  bool persisted = false;

  // When the code cache is produced after execution (the default), the
  // entry keeps the compiled code alive until then: the CommonJS wrapper
  // function, or the unbound script of an ES module. V8 compiles functions
  // lazily, so a cache produced right after compilation only holds the top
  // level; one produced after the code ran also holds the functions it
  // called.
  v8::Global<v8::Function> function;
  v8::Global<v8::UnboundModuleScript> module_script;
  // Size of the largest cache known for this entry: the one read from disk
  // and accepted by V8, or the largest one produced in this process. A new
  // cache is only kept if it is larger.
  uint32_t best_size = 0;
  // The handler's major GC count when the code was compiled; see
  // CompileCacheHandler::Checkpoint().
  uint32_t compiled_at_gc = 0;
  bool retained = false;
  // V8 rejected the cache read from disk, so the file must be replaced even
  // if it is not smaller than the new cache.
  bool rejected = false;

  // Copy the cache into a new store for V8 to consume. Caller takes
  // ownership.
  v8::ScriptCompiler::CachedData* CopyCache() const;
  const char* type_name() const;
};

#define COMPILE_CACHE_STATUS(V)                                                \
  V(FAILED)          /* Failed to enable the cache */                          \
  V(ENABLED)         /* Was not enabled before, and now enabled. */            \
  V(ALREADY_ENABLED) /* Was already enabled. */                                \
  V(DISABLED)        /* Has been disabled by NODE_DISABLE_COMPILE_CACHE. */

enum class CompileCacheEnableStatus : uint8_t {
#define V(status) status,
  COMPILE_CACHE_STATUS(V)
#undef V
};

struct CompileCacheEnableResult {
  CompileCacheEnableStatus status;
  std::string cache_directory;
  std::string message;  // Set in case of failure.
};

enum class EnableOption : uint8_t { DEFAULT, PORTABLE };

// When the V8 code cache of a module is produced.
enum class CaptureMode : uint8_t {
  // Right after the module is compiled, before it runs (the behavior before
  // kExecution existed). Selected with NODE_COMPILE_CACHE_CAPTURE=compile.
  kCompile,
  // After the module ran: at a checkpoint a few major GCs after it was
  // compiled, and again when the cache is persisted (process exit or
  // module.flushCompileCache()).
  kExecution,
};

class CompileCacheHandler {
 public:
  explicit CompileCacheHandler(Environment* env);
  ~CompileCacheHandler();
  CompileCacheHandler(const CompileCacheHandler&) = delete;
  CompileCacheHandler& operator=(const CompileCacheHandler&) = delete;
  CompileCacheEnableResult Enable(Environment* env,
                                  const std::string& dir,
                                  EnableOption option = EnableOption::DEFAULT);

  void Persist();

  CompileCacheEntry* GetOrInsert(v8::Local<v8::String> code,
                                 v8::Local<v8::String> filename,
                                 CachedCodeType type);
  void MaybeSave(CompileCacheEntry* entry,
                 v8::Local<v8::Function> func,
                 bool rejected);
  void MaybeSave(CompileCacheEntry* entry,
                 v8::Local<v8::Module> mod,
                 bool rejected);
  void MaybeSave(CompileCacheEntry* entry, std::string_view transpiled);
  std::string_view cache_dir() { return compile_cache_dir_; }

 private:
  void ReadCacheFile(CompileCacheEntry* entry);
  // Size of the cache in the entry's file on disk if it holds a cache for
  // the same source, 0 otherwise.
  uint32_t OnDiskCacheSize(CompileCacheEntry* entry);
  // Write entry->cache to disk. Returns true if the file was written.
  bool WriteCacheFile(CompileCacheEntry* entry);

  template <typename T>
  void MaybeSaveImpl(CompileCacheEntry* entry,
                     v8::Local<T> func_or_mod,
                     bool rejected);
  void Retain(CompileCacheEntry* entry, v8::Local<v8::Function> func);
  void Retain(CompileCacheEntry* entry, v8::Local<v8::Module> mod);
  // Produce a code cache from the retained code. Keeps it in entry->cache
  // and returns true if it is larger than the best one known for the entry.
  bool Capture(CompileCacheEntry* entry);
  // Capture and write the entries compiled at least checkpoint_gcs_ major
  // GCs ago, so that functions that ran are captured before V8 flushes their
  // bytecode (which it does after --bytecode-old-age major GCs without
  // running them) and before the process is killed without running exit
  // handlers.
  void Checkpoint();
  static void OnGCPrologue(v8::Isolate* isolate,
                           v8::GCType type,
                           v8::GCCallbackFlags flags,
                           void* data);

  template <typename... Args>
  inline void Debug(const char* format, Args&&... args) const;

  static constexpr size_t kMagicNumberOffset = 0;
  static constexpr size_t kCodeSizeOffset = 1;
  static constexpr size_t kCacheSizeOffset = 2;
  static constexpr size_t kCodeHashOffset = 3;
  static constexpr size_t kCacheHashOffset = 4;
  static constexpr size_t kHeaderCount = 5;

  Environment* env_ = nullptr;
  v8::Isolate* isolate_ = nullptr;
  bool is_debug_ = false;

  std::string compile_cache_dir_;
  std::string normalized_compile_cache_dir_;
  EnableOption portable_ = EnableOption::DEFAULT;
  std::unordered_map<uint32_t, std::unique_ptr<CompileCacheEntry>>
      compiler_cache_store_;

  CaptureMode capture_mode_ = CaptureMode::kExecution;
  // Re-produce the cache of one in refresh_interval_ of the entries whose
  // cache from disk was accepted, so a cache made by a run that exercised
  // less code can grow. Serializing a cache costs about as much as consuming
  // it, so refreshing every entry in every process adds up; each process
  // picks a different subset (refresh_seed_), so with many processes or runs
  // every entry still gets refreshed. 1 refreshes all, 0 none. Set with
  // NODE_COMPILE_CACHE_REFRESH=n.
  uint32_t refresh_interval_ = 8;
  uint32_t refresh_seed_ = 0;
  // Major GCs after compilation at which an entry is checkpointed; 0 turns
  // checkpoints off. Set with NODE_COMPILE_CACHE_CHECKPOINT_GCS.
  uint32_t checkpoint_gcs_ = 2;
  uint32_t mark_compacts_ = 0;
  bool gc_callback_added_ = false;
  bool checkpoint_scheduled_ = false;
  // Retained entries not checkpointed yet, in compilation order.
  std::deque<CompileCacheEntry*> pending_checkpoint_;
};
}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_COMPILE_CACHE_H_
