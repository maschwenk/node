'use strict';

// 1,000 sequential process.forkSelf() calls: the parent stays healthy (its V8
// threads are stopped and restarted every time, its children are reaped), and
// every child runs JavaScript and its event loop.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const { forkChild } = require('../common/fork-self');

const kForks = 1000;

(async () => {
  let objects = [];
  const start = process.hrtime.bigint();
  for (let i = 0; i < kForks; i++) {
    // Keep the parent's heap and JIT busy between forks.
    objects.push({ i, s: `x${i}`.repeat(10) });
    if (objects.length > 500) objects = objects.slice(250);
    const child = forkChild(() => new Promise((resolve) => {
      setImmediate(() => resolve(i % 256 === 255 ? 0 : i % 256));
    }));
    if (!child) return;
    const { code, signal } = await child;
    assert.strictEqual(signal, null);
    assert.strictEqual(code, i % 256 === 255 ? 0 : i % 256);
  }
  const ms = Number(process.hrtime.bigint() - start) / 1e6;
  console.log(`${kForks} sequential forks in ${ms.toFixed(0)} ms`);
  if (common.isLinux) {
    // No zombies left behind, and the parent has its usual set of threads.
    const children = fs.readFileSync(`/proc/${process.pid}/task/${process.pid}/children`, 'utf8').trim();
    assert.strictEqual(children, '');
    const threads = fs.readdirSync('/proc/self/task').length;
    assert.ok(threads < 32, `${threads} threads`);
  }
})().then(common.mustCall());
