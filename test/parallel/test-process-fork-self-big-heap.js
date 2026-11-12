// Flags: --max-old-space-size=3072 --expose-gc
'use strict';

// process.forkSelf() with a large heap (1 GiB by default, override with
// NODE_TEST_FORK_SELF_HEAP_MB): the child sees the parent's objects, can
// mutate them without the parent noticing, and can collect garbage over the
// whole heap.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}
if (common.isPi()) common.skip('Too slow for Raspberry Pi devices');

const assert = require('assert');
const v8 = require('v8');
const { forkChild, expectCleanExit } = require('../common/fork-self');

const targetMiB = Number(process.env.NODE_TEST_FORK_SELF_HEAP_MB || 1024);

// ~1 KiB per entry: a string and a small object graph.
const chunks = [];
while (v8.getHeapStatistics().used_heap_size < targetMiB * 1024 * 1024) {
  const chunk = [];
  for (let i = 0; i < 1024; i++) {
    chunk.push({ id: chunks.length * 1024 + i, s: `${i}`.padStart(900, 'x') + chunks.length, next: null });
  }
  for (let i = 1; i < chunk.length; i++) chunk[i - 1].next = chunk[i];
  chunks.push(chunk);
}
const used = v8.getHeapStatistics().used_heap_size;
console.log(`parent heap: ${(used / 2 ** 20).toFixed(0)} MiB in ${chunks.length} chunks`);

(async () => {
  const t0 = process.hrtime.bigint();
  const child = forkChild(() => {
    // Everything is there.
    let n = 0;
    for (const chunk of chunks) {
      for (let node = chunk[0]; node !== null; node = node.next) n++;
    }
    assert.strictEqual(n, chunks.length * 1024);
    // Mutate and collect over the whole heap.
    for (let i = 0; i < chunks.length; i += 2) chunks[i] = null;
    globalThis.gc();
    const after = v8.getHeapStatistics().used_heap_size;
    assert.ok(after < used, `${after} >= ${used}`);
  });
  if (!child) return;
  const forkMs = Number(process.hrtime.bigint() - t0) / 1e6;
  console.log(`forkSelf() with that heap: ${forkMs.toFixed(1)} ms in the parent`);
  await expectCleanExit(child);
  // The parent's objects are untouched.
  assert.ok(chunks.every((c) => c !== null));
  assert.strictEqual(chunks[0][0].id, 0);
})().then(common.mustCall());
