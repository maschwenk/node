// Used by test-compile-cache-after-execution.js. late() is only called after
// the module has been evaluated, when it is named on the command line.

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

atLoad();
setImmediate(() => {
  if (process.argv.includes('late')) late();
});
