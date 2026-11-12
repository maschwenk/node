'use strict';

// Used by test-compile-cache-after-execution.js. late() is only called after
// the module has been loaded, when it is named on the command line.

// V8 compiles a function in parentheses eagerly, together with the top level.
const eager = (function() {
  let sum = 0;
  for (let i = 0; i < 10; i++) sum += i * 2;
  return [sum].map((x) => x + 1);
})();

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
function atLoad() { return body(1).acc + [1, 2, 3].map((x) => x * 2).length; }
function late() { return body(3).acc + [4, 5, 6].map((x) => x * 3).length; }

atLoad(eager);
const args = process.argv.slice(2);
setImmediate(() => {
  if (args.includes('late')) late();
  // With --stress-flush-code, this flushes the bytecode of every function
  // that is not running, as V8 does after a few minutes without running it.
  if (args.includes('gc')) globalThis.gc();
  if (args.includes('wait')) {
    // Stay alive until the parent closes stdin.
    process.stdin.resume();
    process.stdout.write('ready\n');
  }
});
