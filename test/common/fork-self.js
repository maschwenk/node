'use strict';

// Helpers for test-process-fork-self-*.js.
//
// Everything that exists before process.forkSelf() also exists in the child,
// including common.mustCall() wrappers, 'exit' listeners and the caller's
// pending code, so these tests make the child report through its exit code
// (an assertion that fails in the child exits it with code 1) and files, and
// the parent checks exit codes.

const assert = require('assert');

// Forks and runs childFn in the child, then exits the child with the code it
// returns (or 0); childFn may be async. In the parent, returns a promise for
// { pid, code, signal }. In the child, returns null: the caller must return
// right away, the rest of its code is the parent's.
function forkChild(childFn) {
  let resolveExit;
  const exited = new Promise((resolve) => { resolveExit = resolve; });
  const pid = process.forkSelf({
    onExit(code, signal) { resolveExit({ pid, code, signal }); },
  });
  if (pid === 0) {
    runInChild(childFn);
    return null;
  }
  assert.ok(Number.isInteger(pid) && pid > 0, `pid ${pid}`);
  return exited;
}

function runInChild(childFn) {
  // The parent's 'exit' listeners (common's mustCall() checks among them)
  // are the parent's business; the child reports through its exit code.
  process.removeAllListeners('exit');
  const self = process.pid;
  // A grandchild inherits this continuation too: only the process that ran
  // childFn exits when it settles.
  const exit = (code) => {
    // After the caller's own continuations (it returns when it gets null).
    setImmediate(() => {
      if (process.pid === self) process.exit(code);
    });
  };
  let result;
  try {
    result = childFn();
  } catch (err) {
    console.error(err);
    exit(1);
    return;
  }
  Promise.resolve(result).then((code) => exit(code ?? 0), (err) => {
    console.error(err);
    exit(1);
  });
}

async function expectCleanExit(promise) {
  const { pid, code, signal } = await promise;
  assert.strictEqual(signal, null, `child ${pid} killed by ${signal}`);
  assert.strictEqual(code, 0, `child ${pid} exited with ${code}`);
}

module.exports = { forkChild, expectCleanExit };
