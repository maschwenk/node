'use strict';

// A zygote started with child_process.fork() forks on request. The IPC
// channel it inherited belongs to it alone: each child closes its copy before
// any event loop turn (no message meant for the zygote may be consumed by a
// child), without emitting 'disconnect', and process.connected is false and
// process.send/process.channel are gone in the child. The zygote keeps
// receiving every message.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const { fork } = require('child_process');
const fs = require('fs');
const path = require('path');
const tmpdir = require('../common/tmpdir');

const kChildren = 6;
const kNoise = 100;

if (process.argv[2] === 'zygote') {
  const dir = process.argv[3];
  let received = 0;
  let exited = 0;
  process.on('message', (m) => {
    received++;
    if (m.cmd === 'fork') {
      const pid = process.forkSelf({
        onExit(code, signal) {
          exited++;
          process.send({ exited: pid, code, signal });
        },
      });
      if (pid === 0) {
        let got = 0;
        let disconnected = false;
        process.on('message', () => got++);
        process.on('disconnect', () => { disconnected = true; });
        // Several loop turns while the parent floods the channel.
        setTimeout(() => {
          fs.writeFileSync(path.join(dir, `child-${m.n}.json`), JSON.stringify({
            got,
            disconnected,
            connected: process.connected,
            send: typeof process.send,
            channel: typeof process.channel,
            disconnect: typeof process.disconnect,
          }));
          process.exit(0);
        }, 300);
        return;
      }
      process.send({ forked: pid, n: m.n });
    } else if (m.cmd === 'count') {
      process.send({ received, exitedCount: exited });
    }
  });
  return;
}

tmpdir.refresh();
const zygote = fork(__filename, ['zygote', tmpdir.path]);
const exits = [];
let forked = 0;
zygote.on('message', common.mustCallAtLeast((m) => {
  if (m.forked) {
    forked++;
    // Flood the channel right after the fork.
    for (let i = 0; i < kNoise; i++) zygote.send({ cmd: 'noise', i });
    if (forked < kChildren) zygote.send({ cmd: 'fork', n: forked });
  } else if (m.exited) {
    exits.push(m);
    if (exits.length === kChildren) zygote.send({ cmd: 'count' });
  } else if (m.received !== undefined) {
    for (const exit of exits) {
      assert.strictEqual(exit.code, 0, JSON.stringify(exit));
    }
    for (let n = 0; n < kChildren; n++) {
      const child = JSON.parse(
        fs.readFileSync(path.join(tmpdir.path, `child-${n}.json`)));
      assert.deepStrictEqual(child, {
        got: 0,
        disconnected: false,
        connected: false,
        send: 'undefined',
        channel: 'undefined',
        disconnect: 'undefined',
      });
    }
    // Every fork request, every noise message and this count request.
    assert.strictEqual(m.received, kChildren * (1 + kNoise) + 1);
    assert.strictEqual(m.exitedCount, kChildren);
    zygote.disconnect();
  }
}));
zygote.on('exit', common.mustCall((code) => assert.strictEqual(code, 0)));
zygote.send({ cmd: 'fork', n: 0 });
