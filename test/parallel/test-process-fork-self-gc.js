// Flags: --expose-gc
'use strict';

// V8's background threads work in a child of process.forkSelf(): garbage
// collection with concurrent marking and sweeping, and concurrent optimizing
// compilation, while the parent keeps doing the same. Forks while the parent
// is in the middle of an incremental marking cycle and has compile jobs
// queued.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const v8 = require('v8');
const { forkChild, expectCleanExit } = require('../common/fork-self');

function churn(rounds) {
  let keep = [];
  for (let r = 0; r < rounds; r++) {
    const a = [];
    for (let i = 0; i < 20_000; i++) a.push({ i, s: `s${i}`, arr: [i, i + 1] });
    keep.push(a);
    if (keep.length > 8) keep = keep.slice(4);
  }
  return keep.length;
}

// Many distinct hot functions, so that several optimizing compile jobs are
// queued for the background threads at once.
function makeHot(n) {
  const fns = [];
  for (let k = 0; k < n; k++) {
    // eslint-disable-next-line no-new-func
    fns.push(new Function('x', `let s = ${k}; for (let i = 0; i < 50; i++) s += (x * i) % ${k + 7}; return s;`));
  }
  let total = 0;
  for (let round = 0; round < 2000; round++) {
    for (const f of fns) total += f(round);
  }
  return total;
}

function threadNames() {
  if (!common.isLinux) return null;
  return fs.readdirSync('/proc/self/task').map((t) => {
    try {
      return fs.readFileSync(`/proc/self/task/${t}/comm`, 'utf8').trim();
    } catch {
      return '';
    }
  });
}

function childWork() {
  const names = threadNames();
  if (names !== null) {
    assert.ok(names.filter((n) => n === 'node-V8Worker').length >= 1, names.join());
    assert.ok(names.includes('DelayedTaskSche'), names.join());
  }
  const before = v8.getHeapStatistics();
  churn(60);
  globalThis.gc();
  makeHot(40);
  churn(30);
  globalThis.gc();
  const after = v8.getHeapStatistics();
  assert.ok(after.total_heap_size > 0 && before.total_heap_size > 0);
}

(async () => {
  // Plain.
  let child = forkChild(childWork);
  if (!child) return;
  await expectCleanExit(child);

  // In the middle of allocation-heavy work (incremental marking and
  // concurrent sweeping likely in progress) and with compile jobs queued.
  for (let i = 0; i < 6; i++) {
    churn(10 + i * 5);
    makeHot(20);
    child = forkChild(childWork);
    if (!child) return;
    // The parent keeps going too.
    churn(20);
    makeHot(10);
    await expectCleanExit(child);
  }

  // Several children working at the same time as the parent.
  const children = [];
  for (let i = 0; i < 4; i++) {
    child = forkChild(childWork);
    if (!child) return;
    children.push(child);
  }
  churn(40);
  for (const c of children) await expectCleanExit(c);
  globalThis.gc();
})().then(common.mustCall());
