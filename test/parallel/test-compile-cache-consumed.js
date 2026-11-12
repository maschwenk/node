'use strict';

// This tests that a module compiled again in the same process, after its
// cache was consumed and released, reads the cache from disk again.

const common = require('../common');
const { spawnSyncAndAssert } = require('../common/child_process');
const assert = require('assert');
const fs = require('fs');
const tmpdir = require('../common/tmpdir');

tmpdir.refresh();
const dir = tmpdir.resolve('.compile_cache_dir');
const modulePath = tmpdir.resolve('module.js');
const script = tmpdir.resolve('script.js');
fs.writeFileSync(modulePath, 'module.exports = 1;', 'utf-8');
fs.writeFileSync(script, `
require(${JSON.stringify(modulePath)});
delete require.cache[${JSON.stringify(modulePath)}];
require(${JSON.stringify(modulePath)});
`, 'utf-8');

function run(stderr) {
  spawnSyncAndAssert(process.execPath, [script], {
    env: {
      ...process.env,
      NODE_DEBUG_NATIVE: 'COMPILE_CACHE',
      NODE_COMPILE_CACHE: dir,
    },
    cwd: tmpdir.path,
  }, { stderr });
}

run(common.mustCall((output) => {
  assert.match(output, /writing cache for .*module\.js.*success/);
  return true;
}));

run(common.mustCall((output) => {
  const read = /reading cache from .* for CommonJS .*module\.js.*success/g;
  const accepted = /module\.js was accepted, keeping the in-memory entry, releasing its buffer/g;
  assert.strictEqual(output.match(read).length, 2);
  assert.strictEqual(output.match(accepted).length, 2);
  assert.match(output, /skip persisting CommonJS .*module\.js because cache was the same/);
  assert.doesNotMatch(output, /writing cache for .*module\.js/);
  return true;
}));
