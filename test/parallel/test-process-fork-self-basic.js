'use strict';

// process.forkSelf(): returns twice; pid and ppid; the event loop, timers,
// promises, setImmediate, fs (sync and async), dns and child_process work in
// the child; exit codes and signals reach the parent's onExit.

const common = require('../common');
if (typeof process.forkSelf !== 'function') {
  common.skip('process.forkSelf() is not available on this platform');
}

const assert = require('assert');
const fs = require('fs');
const dns = require('dns');
const net = require('net');
const { spawnSync, execFile } = require('child_process');
const { forkChild, expectCleanExit } = require('../common/fork-self');

const parentPid = process.pid;

(async () => {
  // Returns twice; pid/ppid.
  let child = forkChild(() => {
    assert.notStrictEqual(process.pid, parentPid);
    assert.strictEqual(process.ppid, parentPid);
    const desc = Object.getOwnPropertyDescriptor(process, 'pid');
    assert.strictEqual(desc.writable, false);
    // The kernel's view: /proc/self is this process.
    if (common.isLinux) {
      assert.strictEqual(Number(fs.readlinkSync('/proc/self')), process.pid);
    }
  });
  if (!child) return;
  await expectCleanExit(child);
  assert.strictEqual(process.pid, parentPid);

  // Event loop: timers, setImmediate, nextTick, promises, async fs (the libuv
  // threadpool is recreated in the child), fs.promises, dns, a TCP server.
  child = forkChild(async () => {
    const order = [];
    await new Promise((resolve) => {
      setTimeout(() => { order.push('timeout'); resolve(); }, 5);
      setImmediate(() => order.push('immediate'));
      process.nextTick(() => order.push('tick'));
      queueMicrotask(() => order.push('microtask'));
    });
    // (tick and microtask order depends on whether this runs in a microtask)
    assert.deepStrictEqual(order.slice(2), ['immediate', 'timeout']);
    assert.deepStrictEqual(order.slice(0, 2).sort(), ['microtask', 'tick']);

    const data = await fs.promises.readFile(__filename, 'utf8');
    assert.match(data, /process\.forkSelf\(\): returns twice/);
    await new Promise((resolve, reject) => {
      fs.stat(__filename, (err, st) => {
        if (err) return reject(err);
        assert.ok(st.size > 0);
        resolve();
      });
    });
    // Many concurrent threadpool requests.
    await Promise.all(Array.from({ length: 64 }, () => fs.promises.stat(__filename)));

    const { address } = await dns.promises.lookup('localhost');
    assert.ok(net.isIP(address));

    await new Promise((resolve) => {
      const server = net.createServer((sock) => sock.end('hi')).listen(0, () => {
        net.connect(server.address().port, '127.0.0.1').on('data', (d) => {
          assert.strictEqual(d.toString(), 'hi');
          server.close(resolve);
        });
      });
    });
  });
  if (!child) return;
  await expectCleanExit(child);

  // child_process in the child.
  child = forkChild(async () => {
    const r = spawnSync(process.execPath, ['-p', '6 * 7']);
    assert.strictEqual(r.status, 0);
    assert.strictEqual(r.stdout.toString().trim(), '42');
    const out = await new Promise((resolve, reject) => {
      execFile(process.execPath, ['-p', 'process.ppid'], (err, stdout) => {
        if (err) return reject(err);
        resolve(stdout.trim());
      });
    });
    assert.strictEqual(Number(out), process.pid);
  });
  if (!child) return;
  await expectCleanExit(child);

  // Exit codes and signals.
  child = forkChild(() => 7);
  if (!child) return;
  let r = await child;
  assert.deepStrictEqual([r.code, r.signal], [7, null]);
  child = forkChild(() => {
    process.kill(process.pid, 'SIGTERM');
    return new Promise(() => {});
  });
  if (!child) return;
  r = await child;
  assert.deepStrictEqual([r.code, r.signal], [null, 'SIGTERM']);
  child = forkChild(() => { throw new Error('thrown in the child (expected)'); });
  if (!child) return;
  r = await child;
  assert.deepStrictEqual([r.code, r.signal], [1, null]);

  // Without onExit the child is still reaped (no zombie left behind).
  const pid = process.forkSelf();
  if (pid === 0) {
    process.removeAllListeners('exit');
    process.exit(0);
  }
  if (common.isLinux) {
    const deadline = Date.now() + 10_000;
    while (fs.existsSync(`/proc/${pid}`) && Date.now() < deadline) {
      await new Promise((resolve) => setTimeout(resolve, 10));
    }
    assert.ok(!fs.existsSync(`/proc/${pid}`), `child ${pid} was not reaped`);
  }
})().then(common.mustCall());
