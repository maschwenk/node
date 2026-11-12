'use strict';

// process.forkSelf() refuses to fork when it cannot be done safely, and
// leaves the process fully working after refusing.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const { spawnSync } = require('child_process');
const { Worker } = require('worker_threads');
const { forkChild, expectCleanExit } = require('../common/fork-self');

if (process.argv[2] === 'cpu-prof') {
  // V8's CPU profiler runs a sampling thread that forkSelf() does not know.
  try {
    process.forkSelf();
    console.log('forked');
  } catch (err) {
    console.log(`${err.code} ${err.message}`);
  }
  return;
}

// Options are validated.
assert.throws(() => process.forkSelf(1), { code: 'ERR_INVALID_ARG_TYPE' });
assert.throws(() => process.forkSelf({ onExit: 1 }), { code: 'ERR_INVALID_ARG_TYPE' });

// Not available on Worker threads.
const w = new Worker(`
  require('worker_threads').parentPort.postMessage(typeof process.forkSelf);
`, { eval: true });
w.on('message', common.mustCall((type) => {
  assert.strictEqual(type, 'undefined');
}));

(async () => {
  // While a Worker thread is alive.
  assert.throws(() => process.forkSelf(), {
    code: 'ERR_FORK_SELF_UNSAFE',
    message: /Worker threads are running/,
  });
  await new Promise((resolve) => w.on('exit', resolve));

  // While a request is in flight on the event loop.
  const pending = fs.promises.readFile(__filename);
  assert.throws(() => process.forkSelf(), {
    code: 'ERR_FORK_SELF_BUSY',
    message: /requests are in flight/,
  });
  await pending;

  // While the inspector is listening.
  if (common.hasInspector) {
    const inspector = require('inspector');
    inspector.open(0, '127.0.0.1', false);
    assert.throws(() => process.forkSelf(), {
      code: 'ERR_FORK_SELF_UNSAFE',
      message: /inspector is listening/,
    });
    inspector.close();
  }

  // While a thread it does not know runs (Linux: it lists the process's
  // threads), here V8's CPU profiler's.
  if (common.isLinux) {
    const tmpdir = require('../common/tmpdir');
    tmpdir.refresh();
    const r = spawnSync(process.execPath,
                        ['--cpu-prof', '--cpu-prof-dir', tmpdir.path,
                         __filename, 'cpu-prof']);
    assert.strictEqual(r.status, 0, r.stderr.toString());
    assert.match(r.stdout.toString(),
                 /^ERR_FORK_SELF_UNSAFE .*threads it does not know .*\(\d+\)/);
  }

  // Everything works again once the condition is gone, in both processes.
  const child = forkChild(async () => {
    await fs.promises.readFile(__filename);
  });
  if (!child) return;
  await expectCleanExit(child);
})().then(common.mustCall());
