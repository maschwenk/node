#include "compile_cache.h"
#include <algorithm>
#include <string>
#include "debug_utils-inl.h"
#include "env-inl.h"
#include "node_file.h"
#include "node_internals.h"
#include "node_version.h"
#include "path.h"
#include "util.h"
#include "zlib.h"

#ifdef NODE_IMPLEMENTS_POSIX_CREDENTIALS
#include <unistd.h>  // getuid
#endif

#ifdef _WIN32
#include <windows.h>
#endif
namespace node {

using v8::Function;
using v8::GCCallbackFlags;
using v8::GCType;
using v8::HandleScope;
using v8::Isolate;
using v8::Local;
using v8::Module;
using v8::ScriptCompiler;
using v8::String;
using v8::UnboundModuleScript;

namespace {
std::string Uint32ToHex(uint32_t crc) {
  std::string str;
  str.reserve(8);

  for (int i = 28; i >= 0; i -= 4) {
    char digit = (crc >> i) & 0xF;
    digit += digit < 10 ? '0' : 'a' - 10;
    str.push_back(digit);
  }

  return str;
}

// TODO(joyeecheung): use other hashes?
uint32_t GetHash(const char* data, size_t size) {
  uLong crc = crc32(0L, Z_NULL, 0);
  return crc32(crc, reinterpret_cast<const Bytef*>(data), size);
}

std::string GetCacheVersionTag() {
  // On platforms where uids are available, use different folders for
  // different users to avoid cache miss due to permission incompatibility.
  // On platforms where uids are not available, bare with the cache miss.
  // This should be fine on Windows, as there local directories tend to be
  // user-specific.
  std::string tag = std::string(NODE_VERSION) + '-' + std::string(NODE_ARCH) +
                    '-' + Uint32ToHex(ScriptCompiler::CachedDataVersionTag());
#ifdef NODE_IMPLEMENTS_POSIX_CREDENTIALS
  tag += '-' + std::to_string(getuid());
#endif
  return tag;
}

uint32_t GetCacheKey(std::string_view filename, CachedCodeType type) {
  uLong crc = crc32(0L, Z_NULL, 0);
  crc = crc32(crc, reinterpret_cast<const Bytef*>(&type), sizeof(type));
  crc = crc32(
      crc, reinterpret_cast<const Bytef*>(filename.data()), filename.length());
  return crc;
}
}  // namespace

template <typename... Args>
inline void CompileCacheHandler::Debug(const char* format,
                                       Args&&... args) const {
  if (is_debug_) [[unlikely]] {
    FPrintF(stderr, format, std::forward<Args>(args)...);
  }
}

ScriptCompiler::CachedData* CompileCacheEntry::CopyCache() const {
  DCHECK_NOT_NULL(cache);
  int cache_size = cache->length;
  uint8_t* data = new uint8_t[cache_size];
  memcpy(data, cache->data, cache_size);
  return new ScriptCompiler::CachedData(
      data, cache_size, ScriptCompiler::CachedData::BufferOwned);
}

// Used for identifying and verifying a file is a compile cache file.
// See comments in CompileCacheHandler::Persist().
constexpr uint32_t kCacheMagicNumber = 0x8adfdbb2;

const char* CompileCacheEntry::type_name() const {
  switch (type) {
    case CachedCodeType::kCommonJS:
      return "CommonJS";
    case CachedCodeType::kESM:
      return "ESM";
    case CachedCodeType::kStrippedTypeScript:
      return "StrippedTypeScript";
    case CachedCodeType::kTransformedTypeScript:
      return "TransformedTypeScript";
    case CachedCodeType::kTransformedTypeScriptWithSourceMaps:
      return "TransformedTypeScriptWithSourceMaps";
    default:
      UNREACHABLE();
  }
}

void CompileCacheHandler::ReadCacheFile(CompileCacheEntry* entry) {
  Debug("[compile cache] reading cache from %s for %s %s...",
        entry->cache_filename,
        entry->type_name(),
        entry->source_filename);

  uv_fs_t req;
  auto defer_req_cleanup = OnScopeLeave([&req]() { uv_fs_req_cleanup(&req); });
  const char* path = entry->cache_filename.c_str();
  uv_file file = uv_fs_open(nullptr, &req, path, O_RDONLY, 0, nullptr);
  if (req.result < 0) {
    // req will be cleaned up by scope leave.
    Debug(" %s\n", uv_strerror(req.result));
    return;
  }
  uv_fs_req_cleanup(&req);

  auto defer_close = OnScopeLeave([file]() {
    uv_fs_t close_req;
    CHECK_EQ(0, uv_fs_close(nullptr, &close_req, file, nullptr));
    uv_fs_req_cleanup(&close_req);
  });

  // Read the headers.
  std::vector<uint32_t> headers(kHeaderCount);
  uv_buf_t headers_buf = uv_buf_init(reinterpret_cast<char*>(headers.data()),
                                     kHeaderCount * sizeof(uint32_t));
  const int r = uv_fs_read(nullptr, &req, file, &headers_buf, 1, 0, nullptr);
  if (r != static_cast<int>(headers_buf.len)) {
    Debug("reading header failed, bytes read %d", r);
    if (req.result < 0 && is_debug_) {
      Debug(", %s", uv_strerror(req.result));
    }
    Debug("\n");
    return;
  }

  Debug("[%d %d %d %d %d]...",
        headers[kMagicNumberOffset],
        headers[kCodeSizeOffset],
        headers[kCacheSizeOffset],
        headers[kCodeHashOffset],
        headers[kCacheHashOffset]);

  if (headers[kMagicNumberOffset] != kCacheMagicNumber) {
    Debug("magic number mismatch: expected %d, actual %d\n",
          kCacheMagicNumber,
          headers[kMagicNumberOffset]);
    return;
  }

  // Check the code size and hash which are already computed.
  if (headers[kCodeSizeOffset] != entry->code_size) {
    Debug("code size mismatch: expected %d, actual %d\n",
          entry->code_size,
          headers[kCodeSizeOffset]);
    return;
  }
  if (headers[kCodeHashOffset] != entry->code_hash) {
    Debug("code hash mismatch: expected %d, actual %d\n",
          entry->code_hash,
          headers[kCodeHashOffset]);
    return;
  }

  // Read the cache, grow the buffer exponentially whenever it fills up.
  size_t offset = headers_buf.len;
  size_t capacity = 4096;  // Initial buffer capacity
  size_t total_read = 0;
  uint8_t* buffer = new uint8_t[capacity];

  while (true) {
    // If there is not enough space to read more data, do a simple
    // realloc here (we don't actually realloc because V8 requires
    // the underlying buffer to be delete[]-able).
    if (total_read == capacity) {
      size_t new_capacity = capacity * 2;
      auto* new_buffer = new uint8_t[new_capacity];
      memcpy(new_buffer, buffer, capacity);
      delete[] buffer;
      buffer = new_buffer;
      capacity = new_capacity;
    }

    uv_buf_t iov = uv_buf_init(reinterpret_cast<char*>(buffer + total_read),
                               capacity - total_read);
    int bytes_read =
        uv_fs_read(nullptr, &req, file, &iov, 1, offset + total_read, nullptr);
    if (req.result < 0) {  // Error.
      // req will be cleaned up by scope leave.
      delete[] buffer;
      Debug(" %s\n", uv_strerror(req.result));
      return;
    }
    uv_fs_req_cleanup(&req);
    if (bytes_read <= 0) {
      break;
    }
    total_read += bytes_read;
  }

  // Check the cache size and hash.
  if (headers[kCacheSizeOffset] != total_read) {
    Debug("cache size mismatch: expected %d, actual %d\n",
          headers[kCacheSizeOffset],
          total_read);
    return;
  }
  uint32_t cache_hash = GetHash(reinterpret_cast<char*>(buffer), total_read);
  if (headers[kCacheHashOffset] != cache_hash) {
    Debug("cache hash mismatch: expected %d, actual %d\n",
          headers[kCacheHashOffset],
          cache_hash);
    return;
  }

  entry->cache.reset(new ScriptCompiler::CachedData(
      buffer, total_read, ScriptCompiler::CachedData::BufferOwned));
  Debug(" success, size=%d\n", total_read);
}

static std::string GetRelativePath(std::string_view path,
                                   std::string_view base) {
// On Windows, the native encoding is UTF-16, so we need to convert
// the paths to wide strings before using std::filesystem::path.
// On other platforms, std::filesystem::path can handle UTF-8 directly.
#ifdef _WIN32
  std::filesystem::path module_path(ConvertUTF8ToWideString(std::string(path)));
  std::filesystem::path base_path(ConvertUTF8ToWideString(std::string(base)));
#else
  std::filesystem::path module_path(path);
  std::filesystem::path base_path(base);
#endif
  std::filesystem::path relative = module_path.lexically_relative(base_path);
  auto u8str = relative.u8string();
  return std::string(u8str.begin(), u8str.end());
}

CompileCacheEntry* CompileCacheHandler::GetOrInsert(Local<String> code,
                                                    Local<String> filename,
                                                    CachedCodeType type) {
  DCHECK(!compile_cache_dir_.empty());

  Environment* env = Environment::GetCurrent(isolate_->GetCurrentContext());
  Utf8Value filename_utf8(isolate_, filename);
  std::string file_path = filename_utf8.ToString();
  // If the portable cache is enabled and it seems possible to compute the
  // relative position from an absolute path, we use the relative position
  // in the cache key.
  if (portable_ == EnableOption::PORTABLE && IsAbsoluteFilePath(file_path)) {
    // Normalize the path to ensure it is consistent.
    std::string normalized_file_path = NormalizeFileURLOrPath(env, file_path);
    if (normalized_file_path.empty()) {
      return nullptr;
    }
    std::string relative_path =
        GetRelativePath(normalized_file_path, normalized_compile_cache_dir_);
    if (!relative_path.empty()) {
      file_path = relative_path;
      Debug("[compile cache] using relative path %s from %s\n",
            file_path,
            compile_cache_dir_);
    }
  }
  uint32_t key = GetCacheKey(file_path, type);

  // TODO(joyeecheung): don't encode this again into UTF8. If we read the
  // UTF8 content on disk as raw buffer (from the JS layer, while watching out
  // for monkey patching), we can just hash it directly.
  Utf8Value code_utf8(isolate_, code);
  uint32_t code_hash = GetHash(code_utf8.out(), code_utf8.length());
  auto loaded = compiler_cache_store_.find(key);

  // TODO(joyeecheung): let V8's in-isolate compilation cache take precedence.
  if (loaded != compiler_cache_store_.end() &&
      loaded->second->code_hash == code_hash) {
    return loaded->second.get();
  }

  // If the code hash mismatches, the code has changed, discard the stale entry
  // and create a new one.
  auto emplaced =
      compiler_cache_store_.emplace(key, std::make_unique<CompileCacheEntry>());
  auto* result = emplaced.first->second.get();

  result->code_hash = code_hash;
  result->code_size = code_utf8.length();
  result->cache_key = key;
  result->cache_filename =
      compile_cache_dir_ + kPathSeparator + Uint32ToHex(key);
  result->source_filename = filename_utf8.ToString();
  result->cache = nullptr;
  result->type = type;
  // The entry may be reused for changed code (emplace() keeps the existing
  // one): what was known about the old code's cache no longer applies.
  result->best_size = 0;
  result->rejected = false;

  // TODO(joyeecheung): if we fail enough times, stop trying for any future
  // files.
  ReadCacheFile(result);

  return result;
}

ScriptCompiler::CachedData* SerializeCodeCache(Local<Function> func) {
  return ScriptCompiler::CreateCodeCacheForFunction(func);
}

ScriptCompiler::CachedData* SerializeCodeCache(Local<Module> mod) {
  return ScriptCompiler::CreateCodeCache(mod->GetUnboundModuleScript());
}

void CompileCacheHandler::Retain(CompileCacheEntry* entry,
                                 Local<Function> func) {
  entry->function.Reset(isolate_, func);
}

void CompileCacheHandler::Retain(CompileCacheEntry* entry, Local<Module> mod) {
  entry->module_script.Reset(isolate_, mod->GetUnboundModuleScript());
}

template <typename T>
void CompileCacheHandler::MaybeSaveImpl(CompileCacheEntry* entry,
                                        Local<T> func_or_mod,
                                        bool rejected) {
  DCHECK_NOT_NULL(entry);
  const bool accepted = entry->cache != nullptr && !rejected;
  Debug("[compile cache] V8 code cache for %s %s was %s, ",
        entry->type_name(),
        entry->source_filename,
        rejected                    ? "rejected"
        : (entry->cache == nullptr) ? "not initialized"
                                    : "accepted");
  if (capture_mode_ == CaptureMode::kCompile) {
    if (accepted) {
      Debug("keeping the in-memory entry\n");
      return;
    }
    Debug("%s the in-memory entry\n",
          entry->cache == nullptr ? "initializing" : "refreshing");

    ScriptCompiler::CachedData* data = SerializeCodeCache(func_or_mod);
    DCHECK_EQ(data->buffer_policy, ScriptCompiler::CachedData::BufferOwned);
    entry->refreshed = true;
    entry->cache.reset(data);
    return;
  }

  if (accepted) {
    // V8 has consumed its own copy (see CopyCache()), only the size is still
    // needed, to decide whether a later cache is worth writing.
    entry->best_size = entry->cache->length;
    entry->cache.reset();
    if (refresh_interval_ == 0 ||
        (entry->cache_key ^ refresh_seed_) % refresh_interval_ != 0) {
      Debug("keeping the in-memory entry\n");
      return;
    }
    Debug("keeping the in-memory entry, refreshing it after execution\n");
  } else {
    if (rejected) {
      entry->cache.reset();
      entry->best_size = 0;
      entry->rejected = true;
    }
    Debug("%s the in-memory entry after execution\n",
          rejected ? "refreshing" : "initializing");
  }
  Retain(entry, func_or_mod);
  if (!entry->retained) {
    entry->retained = true;
    entry->compiled_at_gc = mark_compacts_;
    if (checkpoint_gcs_ > 0) pending_checkpoint_.push_back(entry);
  }
}

// Whether a cache of new_size bytes is worth replacing one of old_size bytes
// for the same code. Caches produced after execution grow with the functions
// that ran, but the size of a cache for the same code also varies by a few
// dozen bytes from run to run, which must not cause a rewrite on every run.
static bool IsLarger(uint32_t new_size, uint32_t old_size) {
  if (old_size == 0) return new_size > 0;
  return new_size > old_size + std::max<uint32_t>(64, old_size / 256);
}

bool CompileCacheHandler::Capture(CompileCacheEntry* entry) {
  ScriptCompiler::CachedData* data = nullptr;
  if (!entry->function.IsEmpty()) {
    data = SerializeCodeCache(entry->function.Get(isolate_));
  } else if (!entry->module_script.IsEmpty()) {
    data = ScriptCompiler::CreateCodeCache(entry->module_script.Get(isolate_));
  } else {
    return false;
  }
  if (data == nullptr) {  // V8 cannot serialize this script (e.g. asm.js).
    Debug("[compile cache] V8 produced no cache for %s %s\n",
          entry->type_name(),
          entry->source_filename);
    return false;
  }
  DCHECK_EQ(data->buffer_policy, ScriptCompiler::CachedData::BufferOwned);
  uint32_t size = static_cast<uint32_t>(data->length);
  if (!IsLarger(size, entry->best_size)) {
    Debug("[compile cache] cache for %s %s after execution is %d bytes, not "
          "larger than %d, dropping it\n",
          entry->type_name(),
          entry->source_filename,
          size,
          entry->best_size);
    delete data;
    return false;
  }
  Debug("[compile cache] cache for %s %s after execution is %d bytes, was %d\n",
        entry->type_name(),
        entry->source_filename,
        size,
        entry->best_size);
  entry->cache.reset(data);
  entry->best_size = size;
  entry->refreshed = true;
  entry->persisted = false;
  return true;
}

void CompileCacheHandler::OnGCPrologue(Isolate* isolate,
                                       GCType type,
                                       GCCallbackFlags flags,
                                       void* data) {
  auto* handler = static_cast<CompileCacheHandler*>(data);
  handler->mark_compacts_++;
  if (handler->checkpoint_scheduled_ || handler->pending_checkpoint_.empty()) {
    return;
  }
  CompileCacheEntry* oldest = handler->pending_checkpoint_.front();
  if (handler->mark_compacts_ - oldest->compiled_at_gc <
      handler->checkpoint_gcs_) {
    return;
  }
  // The code cache cannot be produced during GC; do it once the current
  // task is done, which also lets a module that is still being evaluated
  // finish first.
  handler->checkpoint_scheduled_ = true;
  handler->env_->SetImmediate(
      [handler](Environment* env) {
        handler->checkpoint_scheduled_ = false;
        handler->Checkpoint();
      },
      CallbackFlags::kUnrefed);
}

void CompileCacheHandler::Checkpoint() {
  if (compile_cache_dir_.empty()) return;
  HandleScope handle_scope(isolate_);
  size_t captured = 0, written = 0;
  while (!pending_checkpoint_.empty()) {
    CompileCacheEntry* entry = pending_checkpoint_.front();
    if (mark_compacts_ - entry->compiled_at_gc < checkpoint_gcs_) break;
    pending_checkpoint_.pop_front();
    if (!Capture(entry)) continue;
    captured++;
    if (WriteCacheFile(entry)) written++;
    // The cache is on disk now; Persist() writes a larger one if the code
    // keeps running. Keep only its size in memory.
    entry->cache.reset();
    entry->refreshed = false;
  }
  Debug("[compile cache] checkpoint at major GC %d: %zu larger caches, %zu "
        "written\n",
        mark_compacts_,
        captured,
        written);
}

void CompileCacheHandler::MaybeSave(CompileCacheEntry* entry,
                                    Local<Module> mod,
                                    bool rejected) {
  DCHECK(mod->IsSourceTextModule());
  MaybeSaveImpl(entry, mod, rejected);
}

void CompileCacheHandler::MaybeSave(CompileCacheEntry* entry,
                                    Local<Function> func,
                                    bool rejected) {
  MaybeSaveImpl(entry, func, rejected);
}

void CompileCacheHandler::MaybeSave(CompileCacheEntry* entry,
                                    std::string_view transpiled) {
  CHECK(entry->type == CachedCodeType::kStrippedTypeScript ||
        entry->type == CachedCodeType::kTransformedTypeScript ||
        entry->type == CachedCodeType::kTransformedTypeScriptWithSourceMaps);
  Debug("[compile cache] saving transpilation cache for %s %s\n",
        entry->type_name(),
        entry->source_filename);

  // TODO(joyeecheung): it's weird to copy it again here. Convert the v8::String
  // directly into buffer held by v8::ScriptCompiler::CachedData here.
  int cache_size = static_cast<int>(transpiled.size());
  uint8_t* data = new uint8_t[cache_size];
  memcpy(data, transpiled.data(), cache_size);
  entry->cache.reset(new ScriptCompiler::CachedData(
      data, cache_size, ScriptCompiler::CachedData::BufferOwned));
  entry->refreshed = true;
}

/**
 * Persist the compile cache accumulated in memory to disk.
 *
 * To avoid race conditions, the cache file includes hashes of the original
 * source code and the cache content. It's first written to a temporary file
 * before being renamed to the target name.
 *
 * Layout of a cache file:
 *   [uint32_t] magic number
 *   [uint32_t] code size
 *   [uint32_t] code hash
 *   [uint32_t] cache size
 *   [uint32_t] cache hash
 *   .... compile cache content ....
 */
uint32_t CompileCacheHandler::OnDiskCacheSize(CompileCacheEntry* entry) {
  uv_fs_t req;
  auto defer_req_cleanup = OnScopeLeave([&req]() { uv_fs_req_cleanup(&req); });
  uv_file file = uv_fs_open(
      nullptr, &req, entry->cache_filename.c_str(), O_RDONLY, 0, nullptr);
  if (req.result < 0) return 0;
  uv_fs_req_cleanup(&req);
  auto defer_close = OnScopeLeave([file]() {
    uv_fs_t close_req;
    CHECK_EQ(0, uv_fs_close(nullptr, &close_req, file, nullptr));
    uv_fs_req_cleanup(&close_req);
  });
  std::vector<uint32_t> headers(kHeaderCount);
  uv_buf_t headers_buf = uv_buf_init(reinterpret_cast<char*>(headers.data()),
                                     kHeaderCount * sizeof(uint32_t));
  const int r = uv_fs_read(nullptr, &req, file, &headers_buf, 1, 0, nullptr);
  if (r != static_cast<int>(headers_buf.len) ||
      headers[kMagicNumberOffset] != kCacheMagicNumber ||
      headers[kCodeSizeOffset] != entry->code_size ||
      headers[kCodeHashOffset] != entry->code_hash) {
    return 0;
  }
  return headers[kCacheSizeOffset];
}

void CompileCacheHandler::Persist() {
  DCHECK(!compile_cache_dir_.empty());

  // TODO(joyeecheung): do this using a separate event loop to utilize the
  // libuv thread pool and do the file system operations concurrently.
  // TODO(joyeecheung): Currently flushing is triggered by either process
  // shutdown or user requests. In the future we should simply start the
  // writes right after module loading on a separate thread, and this method
  // only blocks until all the pending writes (if any) on the other thread are
  // finished. In that case, the off-thread writes should finish long
  // before any attempt of flushing is made so the method would then only
  // incur a negligible overhead from thread synchronization.
  HandleScope handle_scope(isolate_);
  for (auto& pair : compiler_cache_store_) {
    auto* entry = pair.second.get();
    const char* type_name = entry->type_name();
    if (entry->retained) {
      Capture(entry);
    }
    if (entry->cache == nullptr) {
      // After execution, a cache that did not grow is not kept in memory.
      Debug("[compile cache] skip persisting %s %s because %s\n",
            type_name,
            entry->source_filename,
            entry->best_size > 0 ? "cache was the same"
                                 : "the cache was not initialized");
      continue;
    }
    if (entry->refreshed == false) {
      Debug(
          "[compile cache] skip persisting %s %s because cache was the same\n",
          type_name,
          entry->source_filename);
      continue;
    }
    if (entry->persisted == true) {
      Debug("[compile cache] skip persisting %s %s because cache was already "
            "persisted\n",
            type_name,
            entry->source_filename);
      continue;
    }
    WriteCacheFile(entry);
  }

  // Clear the map at the end in one go instead of during the iteration to
  // avoid rehashing costs.
  Debug("[compile cache] Clear deserialized cache.\n");
  pending_checkpoint_.clear();
  compiler_cache_store_.clear();
}

bool CompileCacheHandler::WriteCacheFile(CompileCacheEntry* entry) {
  const char* type_name = entry->type_name();
  {
    DCHECK_EQ(entry->cache->buffer_policy,
              ScriptCompiler::CachedData::BufferOwned);
    char* cache_ptr =
        reinterpret_cast<char*>(const_cast<uint8_t*>(entry->cache->data));
    uint32_t cache_size = static_cast<uint32_t>(entry->cache->length);

    // Other processes may have written a cache for the same code since this
    // one read it (e.g. parallel workers exiting at once). Caches produced
    // after execution only grow with the code that ran, so keep a file that
    // is at least as large. A rejected cache is always replaced.
    if (entry->type != CachedCodeType::kStrippedTypeScript &&
        capture_mode_ == CaptureMode::kExecution && !entry->rejected) {
      uint32_t on_disk = OnDiskCacheSize(entry);
      if (on_disk > 0 && !IsLarger(cache_size, on_disk)) {
        Debug("[compile cache] skip persisting %s %s because the cache on "
              "disk is as large (%d bytes, this one %d)\n",
              type_name,
              entry->source_filename,
              on_disk,
              cache_size);
        entry->persisted = true;
        return false;
      }
    }
    uint32_t cache_hash = GetHash(cache_ptr, cache_size);

    // Generating headers.
    std::vector<uint32_t> headers(kHeaderCount);
    headers[kMagicNumberOffset] = kCacheMagicNumber;
    headers[kCodeSizeOffset] = entry->code_size;
    headers[kCacheSizeOffset] = cache_size;
    headers[kCodeHashOffset] = entry->code_hash;
    headers[kCacheHashOffset] = cache_hash;

    // Generate the temporary filename.
    // The temporary file should be placed in a location like:
    //
    // $NODE_COMPILE_CACHE_DIR/v23.0.0-pre-arm64-5fad6d45-501/e7f8ef7f.cache.tcqrsK
    //
    // 1. $NODE_COMPILE_CACHE_DIR either comes from the $NODE_COMPILE_CACHE
    // environment
    //    variable or `module.enableCompileCache()`.
    // 2. v23.0.0-pre-arm64-5fad6d45-501 is the sub cache directory and
    //    e7f8ef7f is the hash for the cache (see
    //    CompileCacheHandler::Enable()),
    // 3. tcqrsK is generated by uv_fs_mkstemp() as a temporary identifier.
    uv_fs_t mkstemp_req;
    auto cleanup_mkstemp =
        OnScopeLeave([&mkstemp_req]() { uv_fs_req_cleanup(&mkstemp_req); });
    std::string cache_filename_tmp = entry->cache_filename + ".XXXXXX";
    Debug("[compile cache] Creating temporary file for cache of %s (%s)...",
          entry->source_filename,
          type_name);
    int err = uv_fs_mkstemp(
        nullptr, &mkstemp_req, cache_filename_tmp.c_str(), nullptr);
    if (err < 0) {
      Debug("failed. %s\n", uv_strerror(err));
      return false;
    }
    Debug(" -> %s\n", mkstemp_req.path);
    Debug("[compile cache] writing cache for %s %s to temporary file %s [%d "
          "%d %d "
          "%d %d]...",
          type_name,
          entry->source_filename,
          mkstemp_req.path,
          headers[kMagicNumberOffset],
          headers[kCodeSizeOffset],
          headers[kCacheSizeOffset],
          headers[kCodeHashOffset],
          headers[kCacheHashOffset]);

    // Write to the temporary file.
    uv_buf_t headers_buf = uv_buf_init(reinterpret_cast<char*>(headers.data()),
                                       headers.size() * sizeof(uint32_t));
    uv_buf_t data_buf = uv_buf_init(cache_ptr, entry->cache->length);
    uv_buf_t bufs[] = {headers_buf, data_buf};

    uv_fs_t write_req;
    auto cleanup_write =
        OnScopeLeave([&write_req]() { uv_fs_req_cleanup(&write_req); });
    err = uv_fs_write(
        nullptr, &write_req, mkstemp_req.result, bufs, 2, 0, nullptr);
    if (err < 0) {
      Debug("failed: %s\n", uv_strerror(err));
      return false;
    }

    uv_fs_t close_req;
    auto cleanup_close =
        OnScopeLeave([&close_req]() { uv_fs_req_cleanup(&close_req); });
    err = uv_fs_close(nullptr, &close_req, mkstemp_req.result, nullptr);

    if (err < 0) {
      Debug("failed: %s\n", uv_strerror(err));
      return false;
    }

    Debug("success\n");

    // Rename the temporary file to the actual cache file.
    uv_fs_t rename_req;
    auto cleanup_rename =
        OnScopeLeave([&rename_req]() { uv_fs_req_cleanup(&rename_req); });
    std::string cache_filename_final = entry->cache_filename;
    Debug("[compile cache] Renaming %s to %s...",
          mkstemp_req.path,
          cache_filename_final);
    err = uv_fs_rename(nullptr,
                       &rename_req,
                       mkstemp_req.path,
                       cache_filename_final.c_str(),
                       nullptr);
    if (err < 0) {
      Debug("failed: %s\n", uv_strerror(err));
      return false;
    }
    Debug("success\n");
    entry->persisted = true;
  }
  return true;
}

CompileCacheHandler::CompileCacheHandler(Environment* env)
    : env_(env),
      isolate_(env->isolate()),
      is_debug_(
          env->enabled_debug_list()->enabled(DebugCategory::COMPILE_CACHE)) {
  std::string value;
  if (credentials::SafeGetenv("NODE_COMPILE_CACHE_CAPTURE", &value, env) &&
      value == "compile") {
    capture_mode_ = CaptureMode::kCompile;
  }
  if (credentials::SafeGetenv("NODE_COMPILE_CACHE_REFRESH", &value, env) &&
      !value.empty()) {
    refresh_interval_ =
        static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
  }
  refresh_seed_ = static_cast<uint32_t>(uv_hrtime() ^ uv_os_getpid());
  if (credentials::SafeGetenv(
          "NODE_COMPILE_CACHE_CHECKPOINT_GCS", &value, env) &&
      !value.empty()) {
    checkpoint_gcs_ =
        static_cast<uint32_t>(strtoul(value.c_str(), nullptr, 10));
  }
}

CompileCacheHandler::~CompileCacheHandler() {
  if (gc_callback_added_) {
    isolate_->RemoveGCPrologueCallback(OnGCPrologue, this);
  }
}

// Directory structure:
// - Compile cache directory (from NODE_COMPILE_CACHE)
//   - $NODE_VERSION-$ARCH-$CACHE_DATA_VERSION_TAG-$UID
//     - $FILENAME_AND_MODULE_TYPE_HASH.cache: a hash of filename + module type
CompileCacheEnableResult CompileCacheHandler::Enable(Environment* env,
                                                     const std::string& dir,
                                                     EnableOption option) {
  std::string cache_tag = GetCacheVersionTag();
  std::string absolute_cache_dir_base = PathResolve(env, {dir});
  std::string cache_dir_with_tag =
      absolute_cache_dir_base + kPathSeparator + cache_tag;
  CompileCacheEnableResult result;
  Debug("[compile cache] resolved path %s + %s -> %s\n",
        dir,
        cache_tag,
        cache_dir_with_tag);

  if (!env->permission()->is_granted(
          env,
          permission::PermissionScope::kFileSystemWrite,
          cache_dir_with_tag)) [[unlikely]] {
    result.message = "Skipping compile cache because write permission for " +
                     cache_dir_with_tag + " is not granted";
    result.status = CompileCacheEnableStatus::FAILED;
    return result;
  }

  if (!env->permission()->is_granted(
          env,
          permission::PermissionScope::kFileSystemRead,
          cache_dir_with_tag)) [[unlikely]] {
    result.message = "Skipping compile cache because read permission for " +
                     cache_dir_with_tag + " is not granted";
    result.status = CompileCacheEnableStatus::FAILED;
    return result;
  }

  fs::FSReqWrapSync req_wrap;
  int err = fs::MKDirpSync(
      nullptr, &(req_wrap.req), cache_dir_with_tag, 0777, nullptr);
  if (is_debug_) {
    Debug("[compile cache] creating cache directory %s...%s\n",
          cache_dir_with_tag,
          err < 0 ? uv_strerror(err) : "success");
  }
  if (err != 0 && err != UV_EEXIST) {
    result.message =
        "Cannot create cache directory: " + std::string(uv_strerror(err));
    result.status = CompileCacheEnableStatus::FAILED;
    return result;
  }

  result.cache_directory = absolute_cache_dir_base;
  compile_cache_dir_ = cache_dir_with_tag;
  portable_ = option;
  if (capture_mode_ == CaptureMode::kExecution &&
      checkpoint_gcs_ > 0 && !gc_callback_added_) {
    isolate_->AddGCPrologueCallback(
        OnGCPrologue, this, v8::kGCTypeMarkSweepCompact);
    gc_callback_added_ = true;
  }
  Debug("[compile cache] producing caches %s\n",
        capture_mode_ == CaptureMode::kCompile ? "at compilation"
                                               : "after execution");
  if (option == EnableOption::PORTABLE) {
    normalized_compile_cache_dir_ =
        NormalizeFileURLOrPath(env, compile_cache_dir_);
  }
  result.status = CompileCacheEnableStatus::ENABLED;
  return result;
}

}  // namespace node
