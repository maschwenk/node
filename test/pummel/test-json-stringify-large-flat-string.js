'use strict';
const common = require('../common');

// JSON.stringify() of a large flat string used to abort the process with
// "Zone Allocation failed", because V8 reserved the worst-case escaped length
// (eight times the input) as one allocation. It has to return the result, or
// throw a RangeError when the result is longer than the maximum string length.
// Refs: https://github.com/nodejs/node/issues/66489

common.skipIf32Bits();
if (!common.enoughTestMem)
  common.skip('Insufficient memory for large string test');

const assert = require('assert');

// JSON.stringify() returns a flat string, and String.prototype.repeat() does
// not, so the inputs are made with JSON.stringify().

{
  // Two-byte string, 134217726 characters: 8 * length * 2 bytes is over 2 GiB.
  const flat = JSON.stringify('€' + 'a'.repeat(134217723));
  assert.strictEqual(flat.length, 134217726);
  assert.strictEqual(JSON.stringify(flat).length, 134217730);
  assert.strictEqual(JSON.stringify({ payload: flat }).length, 134217742);
  assert.strictEqual(JSON.stringify({ [flat]: 1 }).length, 134217734);
}

{
  // One-byte string, 268435451 characters: 8 * length bytes is over 2 GiB.
  const flat = JSON.stringify('a'.repeat(268435449));
  assert.strictEqual(flat.length, 268435451);
  const result = JSON.stringify(flat);
  assert.strictEqual(result.length, 268435455);
  assert.strictEqual(result.slice(0, 4), '"\\"a');
  assert.strictEqual(result.slice(-4), 'a\\""');
  assert.strictEqual(JSON.stringify([flat]).length, 268435457);
  assert.strictEqual(JSON.stringify({ [flat]: 1 }).length, 268435459);
}

{
  // The result would have 600000006 characters.
  const flat = JSON.stringify('"'.repeat(149999999));
  assert.strictEqual(flat.length, 300000000);
  assert.throws(() => JSON.stringify(flat), {
    name: 'RangeError',
    message: 'Invalid string length',
  });
}
