'use strict';

// After the first TLS use, Node.js parses the CA certificates on a one-shot
// background thread and joins it at exit. A child of process.forkSelf() must
// neither inherit that thread half way through (the certificate caches would
// stay half initialized) nor try to join it at exit (it is the parent's).

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}
if (!common.hasCrypto) common.skip('missing crypto');

const assert = require('assert');
const tls = require('tls');
const fixtures = require('../common/fixtures');
const { forkChild, expectCleanExit } = require('../common/fork-self');

// Starts the certificate loading thread; fork right away, while it may still
// be running.
tls.createSecureContext();

(async () => {
  for (const how of ['exit', 'reallyExit']) {
    const child = forkChild(async () => {
      // The default CA store works in the child.
      const ctx = tls.createSecureContext();
      assert.ok(ctx.context);
      assert.ok(tls.rootCertificates.length > 0);
      // And a TLS connection works end to end.
      await new Promise((resolve, reject) => {
        const server = tls.createServer({
          key: fixtures.readKey('agent1-key.pem'),
          cert: fixtures.readKey('agent1-cert.pem'),
        }, (socket) => socket.end('hello')).listen(0, () => {
          const client = tls.connect({
            port: server.address().port,
            rejectUnauthorized: false,
          }, () => {
            client.on('data', (d) => {
              assert.strictEqual(d.toString(), 'hello');
              client.end();
              server.close(resolve);
            });
          });
          client.on('error', reject);
        });
      });
      if (how === 'reallyExit') process.reallyExit(0);
    });
    if (!child) return;
    await expectCleanExit(child);
  }
})().then(common.mustCall());
