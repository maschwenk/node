'use strict';

// Children of process.forkSelf() must not share randomness with the parent or
// with each other: crypto.randomBytes (OpenSSL's DRBG notices the pid change
// and reseeds), Node's randomUUID and randomInt caches, getRandomValues, and
// Math.random in the main context and in vm contexts (V8's per-context state
// and the isolate's generator are reseeded).

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}
if (!common.hasCrypto) common.skip('missing crypto');

const assert = require('assert');
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');
const vm = require('vm');
const tmpdir = require('../common/tmpdir');
const { forkChild, expectCleanExit } = require('../common/fork-self');

tmpdir.refresh();

const context = vm.createContext({});

function sample() {
  return {
    bytes: crypto.randomBytes(16).toString('hex'),
    uuid: crypto.randomUUID(),
    uuidUnbuffered: crypto.randomUUID({ disableEntropyCache: true }),
    int: crypto.randomInt(2 ** 47),
    values: Buffer.from(crypto.getRandomValues(new Uint8Array(16))).toString('hex'),
    math: Math.random(),
    vmMath: vm.runInContext('Math.random()', context),
  };
}

// Fill every cache in the parent first, so that a child that kept them would
// hand out the same values as its siblings.
const parent = sample();

(async () => {
  const kChildren = 4;
  const exits = [];
  for (let i = 0; i < kChildren; i++) {
    const child = forkChild(() => {
      fs.writeFileSync(path.join(tmpdir.path, `child-${i}.json`),
                       JSON.stringify(sample()));
    });
    if (!child) return;
    exits.push(child);
  }
  for (const exit of exits) await expectCleanExit(exit);
  const samples = [parent, ...Array.from({ length: kChildren }, (_, i) =>
    JSON.parse(fs.readFileSync(path.join(tmpdir.path, `child-${i}.json`))))];
  for (const key of Object.keys(parent)) {
    const values = new Set(samples.map((s) => s[key]));
    assert.strictEqual(values.size, samples.length,
                       `${key} repeated across processes: ${[...values]}`);
  }
})().then(common.mustCall());
