'use strict';

// Long sourceMappingURL magic comments, such as megabytes of inline source
// map, are reported in full and unchanged by vm.Script, after a code cache
// round trip and through the inspector.

const common = require('../common');
const assert = require('assert');
const vm = require('vm');

const base64 = Buffer.alloc(3 * 1024 * 1024, 'source map ').toString('base64');
const url = `data:application/json;charset=utf-8;base64,${base64}`;
const code = '(function(a) { return a + 1; })(41);\n';
const twoByteCode = '"\u2603".length + 41;\n';

let count = 0;
function sourceMapURL(source) {
  return new vm.Script(source, { filename: `long-url-${count++}.js` })
    .sourceMapURL;
}

// One-byte and two-byte sources, `#` and `@`, with and without a trailing
// line terminator or whitespace.
for (const prefix of [code, twoByteCode]) {
  for (const sign of ['#', '@']) {
    for (const suffix of ['', '\n', '\r\n', '  \t\n', '\u2028', ' \u00a0']) {
      const source = `${prefix}//${sign} sourceMappingURL=${url}${suffix}`;
      assert.strictEqual(sourceMapURL(source), url);
    }
  }
}

// Something other than whitespace after the value drops it.
assert.strictEqual(
  sourceMapURL(`${code}//# sourceMappingURL=${url} junk\n`), undefined);
assert.strictEqual(
  sourceMapURL(`${code}//# sourceMappingURL=${url}\u00a0junk\n`), undefined);

// The value ends at the first whitespace or line terminator.
assert.strictEqual(
  sourceMapURL(`${code}//# sourceMappingURL=${url}\u2029junk\n`), url);
assert.strictEqual(
  sourceMapURL(`${code}//# sourceMappingURL=${url}\u3000\n`), url);

// Non-Latin-1 code units inside a long value are kept.
{
  const value = `${url.slice(0, 5000)}\u03bb${url.slice(5000, 10000)}\ud83d\ude00`;
  assert.strictEqual(sourceMapURL(`${code}//# sourceMappingURL=${value}`),
                     value);
  assert.strictEqual(sourceMapURL(`${twoByteCode}//# sourceMappingURL=${value}`),
                     value);
}

// The last comment wins, long or short.
assert.strictEqual(
  sourceMapURL(`${code}//# sourceMappingURL=${url}\n` +
               '//# sourceMappingURL=short.js.map\n'),
  'short.js.map');
assert.strictEqual(
  sourceMapURL(`${code}//# sourceMappingURL=short.js.map\n` +
               `//# sourceMappingURL=${url}\n`),
  url);

// A long value that is a small part of a large source.
{
  const longCode = `${code}// ${'x'.repeat(4 * url.length)}\n`;
  assert.strictEqual(sourceMapURL(`${longCode}//# sourceMappingURL=${url}`),
                     url);
}

// The script runs, and the URL survives a code cache round trip.
{
  const source = `${code}//# sourceMappingURL=${url}\n`;
  const script = new vm.Script(source, { filename: 'long-url-cache-a.js' });
  assert.strictEqual(script.runInThisContext(), 42);
  const cachedData = script.createCachedData();
  const cached = new vm.Script(source, {
    filename: 'long-url-cache-b.js',
    cachedData,
  });
  assert.strictEqual(cached.cachedDataRejected, false);
  assert.strictEqual(cached.sourceMapURL, url);
}

// The inspector reports the same URL. It only reports scripts compiled in a
// context, so use eval rather than vm.Script.
if (common.hasInspector) {
  const { Session } = require('inspector');
  const session = new Session();
  session.connect();
  const parsed = new Map();
  session.on('Debugger.scriptParsed', ({ params }) => {
    parsed.set(params.url, params.sourceMapURL);
  });
  session.post('Debugger.enable', common.mustSucceed(() => {
    (0, eval)(`${code}//# sourceURL=long-url-inspector.js\n` +
              `//# sourceMappingURL=${url}\n`);
    (0, eval)(`${twoByteCode}//# sourceURL=long-url-inspector-two-byte.js\n` +
              `//# sourceMappingURL=${url}\n`);
    setImmediate(common.mustCall(() => {
      assert.strictEqual(parsed.get('long-url-inspector.js'), url);
      assert.strictEqual(parsed.get('long-url-inspector-two-byte.js'), url);
      session.disconnect();
    }));
  }));
}
