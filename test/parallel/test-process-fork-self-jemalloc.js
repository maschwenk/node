'use strict';

// process.forkSelf() in a process that runs with jemalloc preloaded
// (LD_PRELOAD=libjemalloc.so.2), with the libuv threadpool, zlib and crypto
// allocating from several threads before and after the fork. Skipped when
// jemalloc is not installed.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}
if (!common.isLinux) common.skip('LD_PRELOAD jemalloc test is Linux only');

const assert = require('assert');
const fs = require('fs');
const { spawnSync } = require('child_process');

if (process.argv[2] === 'child') {
  const zlib = require('zlib');
  const crypto = require('crypto');
  const { forkChild, expectCleanExit } = require('../common/fork-self');
  const maps = fs.readFileSync('/proc/self/maps', 'utf8');
  assert.match(maps, /libjemalloc/);

  const busy = () => Promise.all([
    ...Array.from({ length: 16 }, () => new Promise((resolve, reject) => {
      zlib.gzip(crypto.randomBytes(256 * 1024), (err, out) => (err ? reject(err) : resolve(out)));
    })),
    ...Array.from({ length: 16 }, () => new Promise((resolve, reject) => {
      crypto.pbkdf2('secret', 'salt', 2000, 64, 'sha256', (err) => (err ? reject(err) : resolve()));
    })),
  ]);

  (async () => {
    for (let i = 0; i < 20; i++) {
      await busy();
      const child = forkChild(async () => {
        await busy();
        const big = Array.from({ length: 2000 }, () => Buffer.alloc(4096, 1));
        assert.strictEqual(big.length, 2000);
      });
      if (!child) return;
      await busy();
      await expectCleanExit(child);
    }
  })().then(common.mustCall());
  return;
}

const candidates = [
  '/usr/lib/aarch64-linux-gnu/libjemalloc.so.2',
  '/usr/lib/x86_64-linux-gnu/libjemalloc.so.2',
  '/usr/lib64/libjemalloc.so.2',
  '/usr/lib/libjemalloc.so.2',
];
const jemalloc = candidates.find((p) => fs.existsSync(p));
if (!jemalloc) common.skip('libjemalloc.so.2 not found');

const r = spawnSync(process.execPath, [__filename, 'child'], {
  env: { ...process.env, LD_PRELOAD: jemalloc },
  stdio: 'inherit',
  timeout: 120_000,
});
assert.strictEqual(r.signal, null);
assert.strictEqual(r.status, 0);
