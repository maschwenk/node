// process.forkSelf(): fork(2) a running Node.js process. The JavaScript side
// is lib/internal/process/fork_self.js; doc/api/process.md describes what is
// and is not handled.
//
// fork(2) copies only the calling thread. Everything else that runs threads
// in a Node.js process is brought to a state where no other thread runs or
// holds a lock at the instant of the fork, and restarted afterwards:
// - V8's platform worker threads and the delayed task scheduler thread are
//   stopped between tasks and joined, then started again in both processes
//   (NodePlatform::StopThreadsForFork()/RestartThreadsAfterFork());
// - the libuv threadpool is idle (no request is in flight on the loop) and is
//   recreated lazily in the child (libuv's atfork handler);
// - the SIGUSR1 watchdog thread takes no lock while waiting and is recreated
//   in the child (inspector::AfterFork());
// - threads that cannot be handled make forkSelf() throw: Worker threads, the
//   inspector's I/O thread, the trace event writer thread and the SIGINT
//   watchdog thread.

#include "env-inl.h"
#include "inspector_agent.h"
#include "node_errors.h"
#include "node_internals.h"
#include "node_platform.h"
#include "node_process.h"
#include "node_v8_platform-inl.h"
#include "node_watchdog.h"
#include "tracing/agent.h"
#include "util-inl.h"
#include "uv.h"
#include "v8-fork.h"
#include "v8.h"
#if HAVE_OPENSSL
#include "crypto/crypto_util.h"
#endif

#if defined(__POSIX__) && !defined(__PASE__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#endif

namespace node {
namespace process {

using v8::Context;
using v8::FunctionCallbackInfo;
using v8::Integer;
using v8::Isolate;
using v8::Local;
using v8::Value;

#if defined(__POSIX__) && !defined(__PASE__)

namespace {

// Returned instead of a pid when the platform threads did not stop in time;
// the JavaScript side lets V8 serve its interrupts and tries again.
constexpr int kForkSelfBusy = -1;

constexpr uint64_t kDefaultStopTimeoutMs = 2000;

void CollectPendingAsyncHandles(uv_handle_t* handle, void* arg) {
  if (handle->type != UV_ASYNC || uv_is_closing(handle)) return;
  uv_async_t* async = reinterpret_cast<uv_async_t*>(handle);
  if (async->pending == 0) return;
  static_cast<std::vector<uv_async_t*>*>(arg)->push_back(async);
}

// The children this process created with forkSelf() and has not reaped yet.
// Nothing else would reap them: libuv only waits for the processes it
// spawned. Each SIGCHLD is answered with waitpid(pid, WNOHANG) for every pid
// here, never waitpid(-1), so the children of child_process are left alone.
// Main thread only.
struct ForkedChildren {
  struct Child {
    pid_t pid;
    // The caller waits for this child's exit: keep the event loop alive.
    bool keep_alive;
  };
  Environment* env;
  uv_signal_t sigchld;
  std::vector<Child> children;
  // forkSelfExitCallback(pid, exitCode, signal), set from JavaScript.
  v8::Global<v8::Function> on_exit;

  void UpdateRef() {
    uv_handle_t* handle = reinterpret_cast<uv_handle_t*>(&sigchld);
    for (const Child& child : children) {
      if (child.keep_alive) return uv_ref(handle);
    }
    uv_unref(handle);
  }
};
ForkedChildren* forked_children = nullptr;

void ReapForkedChildren(uv_signal_t* handle, int signum) {
  ForkedChildren* children = forked_children;
  if (children == nullptr) return;
  Environment* env = children->env;
  struct Exit {
    pid_t pid;
    int exit_code;  // -1 when killed by a signal
    int signal;     // 0 when it exited
  };
  std::vector<Exit> exits;
  for (auto it = children->children.begin(); it != children->children.end();) {
    int status = 0;
    pid_t r;
    do {
      r = waitpid(it->pid, &status, WNOHANG);
    } while (r == -1 && errno == EINTR);
    if (r == 0) {
      ++it;
      continue;
    }
    if (r == it->pid) {
      if (WIFEXITED(status)) {
        exits.push_back({r, WEXITSTATUS(status), 0});
      } else if (WIFSIGNALED(status)) {
        exits.push_back({r, -1, WTERMSIG(status)});
      } else {
        ++it;  // Stopped or continued: still alive.
        continue;
      }
    }
    // Reaped above, or (ECHILD) by someone else.
    it = children->children.erase(it);
  }
  children->UpdateRef();
  if (exits.empty() || children->on_exit.IsEmpty() || !env->can_call_into_js())
    return;
  Isolate* isolate = env->isolate();
  v8::HandleScope handle_scope(isolate);
  Context::Scope context_scope(env->context());
  Local<v8::Function> on_exit = children->on_exit.Get(isolate);
  for (const Exit& exit : exits) {
    Local<Value> argv[] = {
        Integer::New(isolate, exit.pid),
        exit.signal == 0 ? Integer::New(isolate, exit.exit_code).As<Value>()
                         : v8::Null(isolate).As<Value>(),
        exit.signal != 0
            ? OneByteString(isolate, signo_string(exit.signal)).As<Value>()
            : v8::Null(isolate).As<Value>(),
    };
    if (MakeCallback(isolate,
                     env->process_object(),
                     on_exit,
                     arraysize(argv),
                     argv,
                     {0, 0})
            .IsEmpty()) {
      return;  // An exception is pending; the rest waits for the next SIGCHLD.
    }
  }
}

ForkedChildren* GetForkedChildren(Environment* env) {
  if (forked_children != nullptr) return forked_children;
  ForkedChildren* children = new ForkedChildren();
  children->env = env;
  CHECK_EQ(0, uv_signal_init(env->event_loop(), &children->sigchld));
  children->sigchld.data = children;
  CHECK_EQ(0,
           uv_signal_start(&children->sigchld, ReapForkedChildren, SIGCHLD));
  // Waiting for children is not a reason to keep the process alive.
  uv_unref(reinterpret_cast<uv_handle_t*>(&children->sigchld));
  env->AddCleanupHook(
      [](void* arg) {
        ForkedChildren* children = static_cast<ForkedChildren*>(arg);
        forked_children = nullptr;
        children->on_exit.Reset();
        uv_close(reinterpret_cast<uv_handle_t*>(&children->sigchld),
                 [](uv_handle_t* handle) {
                   delete static_cast<ForkedChildren*>(handle->data);
                 });
      },
      children);
  forked_children = children;
  return children;
}

// The threads of this process other than the calling one that fork(2) cannot
// leave behind safely, by name. Known to be fine: the SIGUSR1 watchdog
// (recreated in the child) and idle libuv threadpool workers (libuv's atfork
// handler recreates the pool; no request is in flight, checked above).
// Linux only; elsewhere the list is empty.
std::string UnknownThreads() {
  std::string unknown;
#ifdef __linux__
  const pid_t self = getpid();
  uv_fs_t req;
  if (uv_fs_scandir(nullptr, &req, "/proc/self/task", 0, nullptr) < 0) {
    uv_fs_req_cleanup(&req);
    return unknown;
  }
  uv_dirent_t ent;
  while (uv_fs_scandir_next(&req, &ent) == 0) {
    const pid_t tid = static_cast<pid_t>(strtol(ent.name, nullptr, 10));
    if (tid == self) continue;
    std::string path = std::string("/proc/self/task/") + ent.name + "/comm";
    char name[64] = {0};
    FILE* f = fopen(path.c_str(), "re");
    if (f == nullptr) continue;  // Exited meanwhile.
    if (fgets(name, sizeof(name), f) == nullptr) name[0] = '\0';
    fclose(f);
    std::string comm(name);
    while (!comm.empty() && (comm.back() == '\n' || comm.back() == ' '))
      comm.pop_back();
    if (comm == "SignalInspector" || comm == "libuv-worker") continue;
    if (!unknown.empty()) unknown += ", ";
    unknown += comm + " (" + ent.name + ")";
  }
  uv_fs_req_cleanup(&req);
#endif
  return unknown;
}

}  // namespace

// setForkSelfExitCallback(fn): fn(pid, exitCode, signal) runs when a child
// created by forkSelf() was reaped.
void SetForkSelfExitCallback(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(env->is_main_thread());
  CHECK(args[0]->IsFunction());
  GetForkedChildren(env)->on_exit.Reset(env->isolate(),
                                        args[0].As<v8::Function>());
}

// forkSelf(stopTimeoutMs, keepAlive): pid in the parent, 0 in the child,
// kForkSelfBusy when V8's worker threads did not finish their tasks within
// stopTimeoutMs. With keepAlive, the parent's event loop stays alive until the
// child was reaped. Throws when forking is not safe.
void ForkSelf(const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);

  if (!env->is_main_thread() || !env->owns_process_state()) {
    return THROW_ERR_FORK_SELF_UNSAFE(
        env, "process.forkSelf() is only available on the main thread");
  }
  if (env->sub_worker_context_count() > 0) {
    return THROW_ERR_FORK_SELF_UNSAFE(
        env,
        "process.forkSelf() cannot fork while Worker threads are running "
        "(this includes the thread of off-thread module customization hooks)");
  }
#if HAVE_INSPECTOR
  if (env->inspector_agent()->IsListening()) {
    return THROW_ERR_FORK_SELF_UNSAFE(
        env,
        "process.forkSelf() cannot fork while the inspector is listening "
        "(--inspect, inspector.open())");
  }
#endif
  if (per_process::v8_platform.tracing_agent_ &&
      per_process::v8_platform.tracing_agent_->IsThreadRunning()) {
    return THROW_ERR_FORK_SELF_UNSAFE(
        env,
        "process.forkSelf() cannot fork while trace events are recorded");
  }
  if (SigintWatchdogHelper::GetInstance()->IsRunning()) {
    return THROW_ERR_FORK_SELF_UNSAFE(
        env,
        "process.forkSelf() cannot fork while a SIGINT watchdog is running "
        "(REPL, vm breakOnSigint)");
  }
  // A request in flight would never complete in the child (threadpool work)
  // or complete in both processes (a pending write would be written twice).
  const unsigned int active_requests = env->event_loop()->active_reqs.count;
  if (active_requests > 0) {
    return THROW_ERR_FORK_SELF_BUSY(
        env,
        "process.forkSelf() cannot fork while %d requests are in flight on "
        "the event loop (file system, DNS, crypto or zlib work, pending "
        "writes, connects); call it again once they have completed",
        static_cast<int>(active_requests));
  }

  NodePlatform* platform = per_process::v8_platform.Platform();
  CHECK_NOT_NULL(platform);
  uint64_t stop_timeout_ms = kDefaultStopTimeoutMs;
  if (args[0]->IsNumber()) {
    const double value = args[0].As<v8::Number>()->Value();
    if (value >= 0) stop_timeout_ms = static_cast<uint64_t>(value);
  }

  // On Linux V8 marks its mappings MADV_DONTFORK (so that child_process
  // spawns do not copy the heap's page tables): the child needs them. Once per
  // process, a few milliseconds the first time.
  v8::MakeMappingsForkable();

  // Nothing buffered by C stdio may be written by both processes.
  fflush(nullptr);

#if HAVE_OPENSSL
  // One-shot threads that parse the CA certificates after the first TLS use.
  crypto::JoinCertificateLoadingThreads();
#endif

  if (!platform->StopThreadsForFork(stop_timeout_ms)) {
    return args.GetReturnValue().Set(kForkSelfBusy);
  }

  // Every thread that is still running now must be one that survives fork(2)
  // as is. Anything else (a native addon's thread, a Node.js thread this
  // code does not know about) would be missing in the child while its state
  // says it runs, possibly holding a lock.
  std::string unknown_threads = UnknownThreads();
  // A thread that was just joined can still be listed for a moment.
  for (int i = 0; i < 20 && !unknown_threads.empty(); i++) {
    uv_sleep(1);
    unknown_threads = UnknownThreads();
  }
  if (!unknown_threads.empty()) {
    platform->RestartThreadsAfterFork();
    return THROW_ERR_FORK_SELF_UNSAFE(
        env,
        "process.forkSelf() cannot fork while threads it does not know how to "
        "handle are running (native addons?): %s",
        unknown_threads);
  }

  // libuv clears the pending flag of every async handle in the child (another
  // thread could have been half way through uv_async_send()). No thread can
  // send now, so this is the exact set the child has to wake up again.
  std::vector<uv_async_t*> pending_async_handles;
  uv_walk(env->event_loop(), CollectPendingAsyncHandles, &pending_async_handles);

#if HAVE_INSPECTOR
  inspector::PrepareForFork();
#endif
  const pid_t pid = fork();
  const int fork_errno = errno;

  if (pid == 0) {
    // In the child, before anything can use the loop or wake it up from
    // another thread: new epoll/io_uring instances, a new async eventfd and a
    // new signal pipe, all watchers re-armed.
    const int err = uv_loop_fork(env->event_loop());
    if (err != 0) {
      FPrintF(stderr, "process.forkSelf(): uv_loop_fork() failed: %s\n",
              uv_strerror(err));
      ABORT();
    }
    for (uv_async_t* async : pending_async_handles) uv_async_send(async);
    // The parent's other children are not this process's to reap.
    if (forked_children != nullptr) {
      forked_children->children.clear();
      forked_children->UpdateRef();
    }
    DisownStdioAfterFork();
    v8::ReseedRandomNumberGeneratorsAfterFork(env->isolate());
    // process.pid was computed once at startup. It is a configurable,
    // read-only data property (see PatchProcessObject()).
    Isolate* isolate = env->isolate();
    Local<Context> context = env->context();
    USE(env->process_object()->DefineOwnProperty(
        context,
        FIXED_ONE_BYTE_STRING(isolate, "pid"),
        Integer::New(isolate, uv_os_getpid()),
        v8::ReadOnly));
  }
#if HAVE_INSPECTOR
  inspector::AfterFork(pid == 0);
#endif
  platform->RestartThreadsAfterFork();

  if (pid < 0) {
    return env->ThrowErrnoException(fork_errno, "fork");
  }
  if (pid > 0) {
    ForkedChildren* children = GetForkedChildren(env);
    children->children.push_back({pid, args[1]->IsTrue()});
    children->UpdateRef();
  }
  args.GetReturnValue().Set(static_cast<int32_t>(pid));
}

#endif  // defined(__POSIX__) && !defined(__PASE__)

}  // namespace process
}  // namespace node
