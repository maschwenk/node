'use strict';

// process.forkSelf(): fork(2) the running process. The native part is
// src/node_process_fork.cc; doc/api/process.md describes what is handled.

const {
  ArrayPrototypePush,
  FunctionPrototypeCall,
  SafeMap,
} = primordials;

const {
  codes: {
    ERR_FORK_SELF_BUSY,
  },
} = require('internal/errors');
const {
  validateFunction,
  validateObject,
} = require('internal/validators');

// Returned by the binding when V8's worker threads did not finish their
// current tasks in time (see kForkSelfBusy in node_process_fork.cc).
const kBusy = -1;
// How long the binding waits for V8's worker threads to finish the tasks
// they are running, per attempt. A V8 job keeps a worker until it runs out of
// work, so this is normally a few milliseconds; it is long only when a worker
// waits for the main thread (for instance for a garbage collection it
// requested), which the next attempt resolves because returning to
// JavaScript lets V8 handle that request.
const kStopTimeoutsMs = [500, 2000, 8000];

// Functions run in the child, synchronously, before forkSelf() returns 0.
const childHooks = [];

/**
 * Registers a function to run in every child created by process.forkSelf(),
 * before forkSelf() returns in the child. For internal state that must not
 * be shared with the parent.
 * @param {() => void} fn
 */
function addForkSelfChildHook(fn) {
  ArrayPrototypePush(childHooks, fn);
}

let warned = false;

function forkSelf(options) {
  if (options !== undefined) {
    validateObject(options, 'options');
    if (options.onExit !== undefined) {
      validateFunction(options.onExit, 'options.onExit');
    }
  }
  if (!warned) {
    warned = true;
    // Synchronously: a warning queued for a later tick would be inherited
    // and printed by the child as well.
    require('internal/process/warning').emitWarningSync(
      'process.forkSelf is an experimental feature and might change at any ' +
      'time', 'ExperimentalWarning');
  }
  const {
    forkSelf: rawForkSelf,
    setForkSelfExitCallback,
  } = internalBinding('process_methods');
  let pid = kBusy;
  for (let i = 0; i < kStopTimeoutsMs.length && pid === kBusy; i++) {
    // With onExit, the caller waits for the child: keep the loop alive.
    pid = rawForkSelf(kStopTimeoutsMs[i], options?.onExit !== undefined);
  }
  if (pid === kBusy) {
    throw new ERR_FORK_SELF_BUSY(
      'process.forkSelf() could not stop V8\'s worker threads: a background ' +
      'task did not finish');
  }
  if (pid === 0) {
    runChildHooks();
    return 0;
  }
  // The binding reaps every child it created (nothing else would); this
  // reports the exit to the caller.
  if (options?.onExit !== undefined) {
    if (!exitCallbackSet) {
      exitCallbackSet = true;
      setForkSelfExitCallback(onChildExit);
    }
    exitCallbacks.set(pid, options.onExit);
  }
  return pid;
}

// pid -> options.onExit of the children of this process.
const exitCallbacks = new SafeMap();
let exitCallbackSet = false;

function onChildExit(pid, code, signal) {
  const onExit = exitCallbacks.get(pid);
  if (onExit === undefined) return;
  exitCallbacks.delete(pid);
  onExit(code, signal);
}

function runChildHooks() {
  // The parent's children are not this process's.
  exitCallbacks.clear();
  // The inherited IPC channel belongs to the parent: close it before any
  // event loop turn could read a message meant for the parent.
  if (process.channel) {
    require('internal/child_process').detachChannelAfterFork(process);
  }
  for (let i = 0; i < childHooks.length; i++) {
    FunctionPrototypeCall(childHooks[i], undefined);
  }
}

module.exports = {
  addForkSelfChildHook,
  forkSelf,
};
