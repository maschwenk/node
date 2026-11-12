'use strict';

// This tests that the compile cache is produced after the code ran, so that
// functions first called after a module was loaded are compiled from the
// cache by the next run instead of lazily.

require('../common');
const { spawnSyncAndAssert } = require('../common/child_process');
const assert = require('assert');
const { spawnSync } = require('child_process');
const fixtures = require('../common/fixtures');
const tmpdir = require('../common/tmpdir');
const fs = require('fs');
const path = require('path');

tmpdir.refresh();
const logFile = tmpdir.resolve('v8.log');
// V8 accepts a cache only if it was produced under the same flags, and the
// name of the log file is one of them, so every run uses the same flags.
const v8Flags = [
  '--log-function-events',
  `--logfile=${logFile}`,
  '--no-logfile-per-isolate',
];
const cjs = fixtures.path('compile-cache-after-execution.js');
const esm = fixtures.path('compile-cache-after-execution.mjs');

// Names of the functions V8 compiled lazily, i.e. not from a cache, in the
// last run.
function lazilyCompiled() {
  const names = new Set();
  for (const line of fs.readFileSync(logFile, 'utf8').split('\n')) {
    const fields = line.split(',');
    if (fields[0] === 'function' && fields[1] === 'interpreter') {
      names.add(fields.at(-1));
    }
  }
  return names;
}

function cacheSize(dir) {
  const [versionDir] = fs.readdirSync(dir);
  const files = fs.readdirSync(path.join(dir, versionDir));
  assert.strictEqual(files.length, 1);
  return fs.statSync(path.join(dir, versionDir, files[0])).size;
}

function run(dir, script, args, stderr, { flags = v8Flags, env = {} } = {}) {
  spawnSyncAndAssert(
    process.execPath,
    [...flags, script, ...args],
    {
      env: {
        ...process.env,
        NODE_DEBUG_NATIVE: 'COMPILE_CACHE',
        NODE_COMPILE_CACHE: dir,
        ...env,
      },
      cwd: tmpdir.path,
    },
    {
      stderr(output) {
        console.log(output);  // Logging for debugging.
        stderr?.(output);
        return true;
      },
    });
  return lazilyCompiled();
}

{
  // CommonJS: the cache holds the functions that ran, grows when a later
  // run calls more of them, and does not shrink when a run calls fewer.
  const dir = tmpdir.resolve('cjs');
  let lazy = run(dir, cjs, ['late'], (output) => {
    assert.match(output, /after-execution\.js was not initialized, initializing the in-memory entry after execution/);
    assert.match(output, /writing cache for CommonJS .*after-execution\.js.*success/);
  });
  assert(lazy.has('atLoad'));
  assert(lazy.has('late'));
  const size1 = cacheSize(dir);

  lazy = run(dir, cjs, ['late'], (output) => {
    assert.match(output, /after-execution\.js was accepted, keeping the in-memory entry/);
    assert.match(output, /skip persisting CommonJS .*after-execution\.js because cache was the same/);
  });
  assert(!lazy.has('atLoad'));
  assert(!lazy.has('late'));
  assert.strictEqual(cacheSize(dir), size1);

  lazy = run(dir, cjs, ['late', 'later'], (output) => {
    assert.match(output, /writing cache for CommonJS .*after-execution\.js.*success/);
  });
  assert(!lazy.has('late'));
  assert(lazy.has('later'));
  const size2 = cacheSize(dir);
  assert(size2 > size1, `${size2} > ${size1}`);

  lazy = run(dir, cjs, [], (output) => {
    assert.match(output, /skip persisting CommonJS .*after-execution\.js because cache was the same/);
  });
  assert(!lazy.has('atLoad'));
  assert.strictEqual(cacheSize(dir), size2);

  lazy = run(dir, cjs, ['late', 'later']);
  assert(!lazy.has('late'));
  assert(!lazy.has('later'));
}

{
  // NODE_COMPILE_CACHE_CAPTURE=compile produces the cache right after
  // compilation, which only holds the top level.
  const dir = tmpdir.resolve('compile');
  const env = { NODE_COMPILE_CACHE_CAPTURE: 'compile' };
  run(dir, cjs, ['late'], null, { env });
  const lazy = run(dir, cjs, ['late'], (output) => {
    assert.match(output, /after-execution\.js was accepted, keeping the in-memory entry/);
  }, { env });
  assert(lazy.has('atLoad'));
  assert(lazy.has('late'));
}

{
  // ES modules.
  const dir = tmpdir.resolve('esm');
  let lazy = run(dir, esm, ['late'], (output) => {
    assert.match(output, /writing cache for ESM .*after-execution\.mjs.*success/);
  });
  assert(lazy.has('late'));
  lazy = run(dir, esm, ['late'], (output) => {
    assert.match(output, /after-execution\.mjs was accepted, keeping the in-memory entry/);
  });
  assert(!lazy.has('atLoad'));
  assert(!lazy.has('late'));
}

{
  // A checkpoint a few major GCs after compilation writes the cache before
  // the process ends, even if it is then killed by a signal.
  const dir = tmpdir.resolve('checkpoint');
  const flags = [...v8Flags, '--expose-gc'];
  const child = spawnSync(process.execPath, [...flags, cjs, 'late', 'checkpoint'], {
    env: {
      ...process.env,
      NODE_DEBUG_NATIVE: 'COMPILE_CACHE',
      NODE_COMPILE_CACHE: dir,
    },
    cwd: tmpdir.path,
  });
  const output = child.stderr.toString();
  console.log(output);
  assert.strictEqual(child.signal, 'SIGKILL');
  assert.match(output, /checkpoint at major GC/);
  assert.match(output, /writing cache for CommonJS .*after-execution\.js.*success/);
  assert(lazilyCompiled().has('late'));

  const lazy = run(dir, cjs, ['late'], null, { flags });
  assert(!lazy.has('late'));
}
