'use strict';

// Used by test-compile-cache-after-execution.js. The functions named on the
// command line are first called after the module has been loaded.

// Large enough that the cache grows measurably when one of them is in it.
function body(n) {
  let acc = 0;
  for (let i = 0; i < n; i++) {
    switch (i % 4) {
      case 0: acc += i; break;
      case 1: acc ^= i * 3; break;
      case 2: acc = Math.imul(acc, 31) + String(i).length; break;
      default: acc -= n;
    }
  }
  return { acc, label: `n=${n}` };
}
function atLoad() { return body(1).acc + body(2).acc + [1, 2, 3].map((x) => x * 2).length; }
function late() { return body(3).acc + [4, 5, 6].map((x) => x * 3).length + body(4).label.length; }
function later() { return body(5).acc + [7, 8, 9].map((x) => x * 4).length + body(6).label.length; }

atLoad();
const calls = new Set(process.argv.slice(2));
setImmediate(() => {
  if (calls.has('late')) late();
  if (calls.has('later')) later();
  if (calls.has('checkpoint')) {
    // Two major GCs make the compile cache checkpoint the module. The
    // process is then killed, so no exit handler can persist anything.
    globalThis.gc();
    globalThis.gc();
    setImmediate(() => process.kill(process.pid, 'SIGKILL'));
  }
});
