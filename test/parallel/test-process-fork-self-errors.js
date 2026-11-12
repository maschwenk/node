'use strict';

// process.forkSelf() refuses to fork when it cannot be done safely, and
// leaves the process fully working after refusing.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const { Worker } = require('worker_threads');
const { forkChild, expectCleanExit } = require('../common/fork-self');

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

  // Everything works again once the condition is gone, in both processes.
  const child = forkChild(async () => {
    await fs.promises.readFile(__filename);
  });
  if (!child) return;
  await expectCleanExit(child);
})().then(common.mustCall());
