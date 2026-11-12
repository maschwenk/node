'use strict';

// A child of process.forkSelf() can fork again (children of children), each
// level reaping its own children, with pids and randomness per process.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const tmpdir = require('../common/tmpdir');
const { forkChild, expectCleanExit } = require('../common/fork-self');

tmpdir.refresh();
const kDepth = 4;

function level(depth) {
  return async () => {
    fs.writeFileSync(path.join(tmpdir.path, `level-${depth}.json`),
                     JSON.stringify({ pid: process.pid, ppid: process.ppid, r: Math.random() }));
    if (depth < kDepth) {
      let child = forkChild(level(depth + 1));
      if (!child) return;
      await expectCleanExit(child);
      // Fork twice at this level, the second one after the first was reaped.
      child = forkChild(() => {});
      if (!child) return;
      await expectCleanExit(child);
    }
  };
}

(async () => {
  const child = forkChild(level(1));
  if (!child) return;
  await expectCleanExit(child);
  const levels = [{ pid: process.pid }];
  for (let d = 1; d <= kDepth; d++) {
    levels.push(JSON.parse(fs.readFileSync(path.join(tmpdir.path, `level-${d}.json`))));
  }
  for (let d = 1; d <= kDepth; d++) {
    assert.strictEqual(levels[d].ppid, levels[d - 1].pid);
  }
  assert.strictEqual(new Set(levels.map((l) => l.r)).size, levels.length);
})().then(common.mustCall());
