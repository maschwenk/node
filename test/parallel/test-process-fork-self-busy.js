// Flags: --expose-internals --expose-gc
'use strict';

// When V8's worker threads do not all park within the binding's timeout,
// forkSelf() lets them continue and reports "busy" (the JavaScript side
// retries). With a zero timeout that path runs on every call (and with 2 ms
// on some): the process must keep working, with all of its worker threads,
// and fork normally afterwards.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const { internalBinding } = require('internal/test/binding');
const { forkChild, expectCleanExit } = require('../common/fork-self');

const { forkSelf: rawForkSelf } = internalBinding('process_methods');
const kBusy = -1;

function workerThreads() {
  if (!common.isLinux) return null;
  return fs.readdirSync('/proc/self/task').filter((t) => {
    try {
      return fs.readFileSync(`/proc/self/task/${t}/comm`, 'utf8').trim() === 'node-V8Worker';
    } catch {
      return false;
    }
  }).length;
}

const workersBefore = workerThreads();
let busy = 0;
let forked = 0;
let keep = [];
for (let i = 0; i < 300; i++) {
  // Give the workers something to do: allocation drives concurrent marking
  // and sweeping.
  for (let j = 0; j < 2000; j++) keep.push({ j, s: `${i}:${j}` });
  if (keep.length > 200000) keep = keep.slice(100000);
  // A zero timeout takes the busy path; 2 ms lets some forks through.
  const pid = rawForkSelf(i % 2 ? 2 : 0, false);
  if (pid === kBusy) {
    busy++;
    continue;
  }
  if (pid === 0) {
    process.removeAllListeners('exit');
    process.reallyExit(0);
  }
  forked++;
}
console.log(`busy ${busy}, forked ${forked}`);
assert.strictEqual(busy + forked, 300);

globalThis.gc();
if (workersBefore !== null) {
  // Every worker thread is running again (a thread that just stopped may
  // linger for a moment).
  assert.strictEqual(workerThreads(), workersBefore);
}

(async () => {
  const child = forkChild(() => {
    const a = [];
    for (let i = 0; i < 1e5; i++) a.push({ i });
    globalThis.gc();
  });
  if (!child) return;
  await expectCleanExit(child);
})().then(common.mustCall());
