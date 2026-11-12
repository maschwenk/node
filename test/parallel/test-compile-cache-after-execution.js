'use strict';

// This tests that the compile cache is produced again from the compiled code
// when it is persisted, after the code ran, so that functions first called
// after a module was loaded come from the cache in the next process instead
// of being compiled lazily again.

const common = require('../common');
const { spawnSyncAndAssert } = require('../common/child_process');
const assert = require('assert');
const { spawn } = require('child_process');
const fixtures = require('../common/fixtures');
const tmpdir = require('../common/tmpdir');
const fs = require('fs');
const os = require('os');
const path = require('path');
const zlib = require('zlib');

tmpdir.refresh();
const cjs = fixtures.path('compile-cache-after-execution.js');
const esm = fixtures.path('compile-cache-after-execution.mjs');
const logFile = tmpdir.resolve('v8.log');
// V8 only accepts a cache produced under the same flags, the name of the log
// file included, so all runs that log use the same flags.
const logFlags = [
  '--log-function-events',
  `--logfile=${logFile}`,
  '--no-logfile-per-isolate',
];

function env(dir) {
  return {
    ...process.env,
    NODE_DEBUG_NATIVE: 'COMPILE_CACHE',
    NODE_COMPILE_CACHE: dir,
  };
}

function run(dir, flags, script, args, stderr) {
  spawnSyncAndAssert(process.execPath, [...flags, script, ...args], {
    env: env(dir),
    cwd: tmpdir.path,
  }, {
    stderr(output) {
      console.log(output);  // Logging for debugging.
      stderr?.(output);
      return true;
    },
  });
}

// Names of the functions that V8 compiled from source (not from a cache) in
// the last run that logged.
function compiledFromSource() {
  const names = new Set();
  for (const line of fs.readFileSync(logFile, 'utf8').split('\n')) {
    const fields = line.split(',');
    if (fields[0] === 'function' && fields[1] === 'interpreter') {
      names.add(fields.at(-1));
    }
  }
  return names;
}

function cacheFile(dir) {
  const [versionDir] = fs.readdirSync(dir);
  const files = fs.readdirSync(path.join(dir, versionDir));
  assert.strictEqual(files.length, 1);
  return path.join(dir, versionDir, files[0]);
}

for (const [type, script] of [['CommonJS', cjs], ['ESM', esm]]) {
  // A function that first ran after the module was loaded is in the cache.
  const dir = tmpdir.resolve(`after-execution-${type}`);
  run(dir, logFlags, script, ['late'], common.mustCall((output) => {
    assert.match(output, new RegExp(`cache for ${type} .*after-execution\\.m?js after execution is \\d+ bytes, \\d+ at compilation, keeping the one after execution`));
    assert.match(output, new RegExp(`writing cache for ${type} .*after-execution\\.m?js.*success`));
  }));
  assert(compiledFromSource().has('late'));

  run(dir, logFlags, script, ['late'], common.mustCall((output) => {
    assert.match(output, /after-execution\.m?js was accepted, keeping the in-memory entry/);
  }));
  const compiled = compiledFromSource();
  assert(!compiled.has('atLoad'));
  assert(!compiled.has('late'));
}

{
  // V8 flushes the bytecode of functions that have not run for a while,
  // including the top level, so in a long-running process the cache produced
  // at compilation can be larger than the one produced at exit, and is kept.
  // --stress-flush-code makes gc() flush all of it.
  const dir = tmpdir.resolve('flushed');
  const flags = ['--expose-gc', '--stress-flush-code'];
  run(dir, flags, cjs, ['late', 'gc'], common.mustCall((output) => {
    assert.match(output, /cache for CommonJS .*after-execution\.js after execution is \d+ bytes, \d+ at compilation, keeping the one from compilation/);
    assert.match(output, /writing cache for CommonJS .*after-execution\.js.*success/);
  }));
  run(dir, flags, cjs, [], common.mustCall((output) => {
    assert.match(output, /after-execution\.js was accepted, keeping the in-memory entry/);
  }));
}

{
  // A cache that V8 rejected is replaced even if the file is larger.
  const dir = tmpdir.resolve('rejected');
  run(dir, [], cjs, ['late']);
  const file = cacheFile(dir);
  const contents = fs.readFileSync(file);
  // Change the magic number of V8's data, after the 5 uint32 of the header
  // that Node.js writes, and update the hash of the data in that header.
  contents[20] ^= 1;
  contents[`writeUInt32${os.endianness()}`](zlib.crc32(contents.subarray(20)), 16);
  fs.writeFileSync(file, contents);
  run(dir, [], cjs, [], common.mustCall((output) => {
    assert.match(output, /after-execution\.js was rejected, refreshing the in-memory entry/);
    assert.match(output, /writing cache for CommonJS .*after-execution\.js.*success/);
  }));
  assert(fs.statSync(file).size < contents.length);
  run(dir, [], cjs, [], common.mustCall((output) => {
    assert.match(output, /after-execution\.js was accepted, keeping the in-memory entry/);
  }));
}

{
  // A process does not replace a cache file with a smaller one written by
  // another process for the same code after it read the directory, e.g. by a
  // process that ran more of the code and exited first.
  const dir = tmpdir.resolve('concurrent');
  const waiting = spawn(process.execPath, [cjs, 'wait'], {
    env: env(dir),
    cwd: tmpdir.path,
    stdio: ['pipe', 'pipe', 'pipe'],
  });
  let stderr = '';
  waiting.stderr.setEncoding('utf8');
  waiting.stderr.on('data', (data) => { stderr += data; });
  waiting.stdout.setEncoding('utf8');
  waiting.stdout.once('data', common.mustCall((data) => {
    assert.strictEqual(data, 'ready\n');
    run(dir, [], cjs, ['late'], common.mustCall((output) => {
      assert.match(output, /writing cache for CommonJS .*after-execution\.js.*success/);
    }));
    const written = fs.readFileSync(cacheFile(dir));
    waiting.on('close', common.mustCall((code) => {
      console.log(stderr);
      assert.strictEqual(code, 0);
      assert.match(stderr, /after-execution\.js was not initialized, initializing the in-memory entry/);
      assert.match(stderr, /skip persisting CommonJS .*after-execution\.js because the cache on disk is as large/);
      assert.deepStrictEqual(fs.readFileSync(cacheFile(dir)), written);
    }));
    waiting.stdin.end();
  }));
}
