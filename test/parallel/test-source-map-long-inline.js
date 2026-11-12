'use strict';

// Megabytes of inline source map are found and applied by
// --enable-source-maps in CommonJS and ES modules, also when the module comes
// from the compile cache, and are written to NODE_V8_COVERAGE output.

require('../common');
const { spawnSyncAndAssert, spawnSyncAndExitWithoutError } =
  require('../common/child_process');
const assert = require('assert');
const fs = require('fs');
const path = require('path');
const tmpdir = require('../common/tmpdir');

tmpdir.refresh();

// Maps generated line 2, column 2 to src.ts line 10, column 5.
const sourcesContent = Array.from(
  { length: 30000 }, (_, i) => `// ${i + 1} ${'x'.repeat(100)}`).join('\n');
const map = {
  version: 3,
  sources: ['src.ts'],
  sourcesContent: [sourcesContent],
  names: [],
  mappings: ';EASI',
};
const comment = '//# sourceMappingURL=data:application/json;base64,' +
  Buffer.from(JSON.stringify(map)).toString('base64');
const code = 'function f() {\n  throw new Error("boom");\n}\n';

const files = {
  'throw.cjs': `${code}f();\n${comment}\n`,
  'throw.mjs': `${code}f();\n${comment}\n`,
  'ok.cjs': `${code}\n${comment}\n`,
};
for (const [name, content] of Object.entries(files)) {
  fs.writeFileSync(tmpdir.resolve(name), content);
}

for (const name of ['throw.cjs', 'throw.mjs']) {
  const cacheDir = tmpdir.resolve(`.compile_cache_${name}`);
  const env = {
    ...process.env,
    NODE_DEBUG_NATIVE: 'COMPILE_CACHE',
    NODE_COMPILE_CACHE: cacheDir,
  };
  for (const run of ['write', 'read']) {
    spawnSyncAndAssert(
      process.execPath,
      ['--enable-source-maps', tmpdir.resolve(name)],
      { env, cwd: tmpdir.path },
      {
        status: 1,
        stderr(output) {
          assert.match(output, /Error: boom/);
          assert.match(output, /src\.ts:10:5/);
          if (run === 'read') {
            assert.match(output, new RegExp(`cache for .*${name.replace('.', '\\.')} was accepted`));
          }
          return true;
        },
      });
  }
}

{
  const coverageDir = tmpdir.resolve('coverage');
  spawnSyncAndExitWithoutError(
    process.execPath,
    [tmpdir.resolve('ok.cjs')],
    { env: { ...process.env, NODE_V8_COVERAGE: coverageDir }, cwd: tmpdir.path });
  const [file] = fs.readdirSync(coverageDir);
  const coverage = JSON.parse(fs.readFileSync(path.join(coverageDir, file)));
  const entry = Object.entries(coverage['source-map-cache'])
    .find(([key]) => key.endsWith('ok.cjs'));
  assert.ok(entry);
  assert.strictEqual(entry[1].url, null);
  assert.strictEqual(entry[1].data.mappings, ';EASI');
  assert.strictEqual(entry[1].data.sourcesContent[0], sourcesContent);
}
