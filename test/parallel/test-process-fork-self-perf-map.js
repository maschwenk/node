// Flags: --perf-basic-prof
'use strict';

// With --perf-basic-prof, a child of process.forkSelf() writes its own
// perf-<pid>.map, which starts with everything the parent had written (the
// code the child inherited) and continues with the child's own code.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}
if (!common.isLinux) common.skip('--perf-basic-prof is Linux only');

const assert = require('assert');
const fs = require('fs');
const { forkChild, expectCleanExit } = require('../common/fork-self');

function hot(name) {
  // eslint-disable-next-line no-new-func
  const f = new Function('x', `return function ${name}(y) { return x + y; }`)(1);
  for (let i = 0; i < 1e5; i++) f(i);
  return f;
}

hot('compiledInTheParent');
const parentMap = `/tmp/perf-${process.pid}.map`;
assert.match(fs.readFileSync(parentMap, 'utf8'), /compiledInTheParent/);

(async () => {
  let childPid;
  const child = forkChild(() => {
    hot('compiledInTheChild');
    fs.writeFileSync(`/tmp/fork-self-perf-map-${process.ppid}`, String(process.pid));
  });
  if (!child) return;
  ({ pid: childPid } = await expectCleanExit(child).then(() => child));
  const childMap = `/tmp/perf-${childPid}.map`;
  const text = fs.readFileSync(childMap, 'utf8');
  assert.match(text, /compiledInTheParent/);
  assert.match(text, /compiledInTheChild/);
  assert.doesNotMatch(fs.readFileSync(parentMap, 'utf8'), /compiledInTheChild/);
  fs.unlinkSync(childMap);
  fs.unlinkSync(`/tmp/fork-self-perf-map-${process.pid}`);
})().then(common.mustCall());
