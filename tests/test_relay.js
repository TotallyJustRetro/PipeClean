'use strict';

const assert = require('node:assert/strict');
const net = require('node:net');
const { spawn } = require('node:child_process');

function waitForLine(client, timeoutMs = 3000) {
  if (client.lines.length) return Promise.resolve(client.lines.shift());
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      const idx = client.waiters.indexOf(waiter);
      if (idx !== -1) client.waiters.splice(idx, 1);
      reject(new Error('Timed out waiting for relay line'));
    }, timeoutMs);
    const waiter = (line) => {
      clearTimeout(timeout);
      resolve(line);
    };
    client.waiters.push(waiter);
  });
}
function connectClient(port) {
  const socket = net.createConnection({ host: '127.0.0.1', port });
  const client = { socket, lines: [], waiters: [], buffer: '' };
  socket.setEncoding('utf8');
  socket.on('data', (chunk) => {
    client.buffer += chunk;
    let newline;
    while ((newline = client.buffer.indexOf('\n')) !== -1) {
      const line = client.buffer.slice(0, newline).replace(/\r$/, '');
      client.buffer = client.buffer.slice(newline + 1);
      const waiter = client.waiters.shift();
      if (waiter) waiter(line);
      else client.lines.push(line);
    }
  });
  return new Promise((resolve, reject) => {
    socket.once('connect', () => resolve(client));
    socket.once('error', reject);
  });
}
async function main() {
  const relay = spawn(process.execPath, ['server/relay.js'], {
    env: { ...process.env, PIPECLEAN_RELAY_PORT: '0', PIPECLEAN_RELAY_BIND: '127.0.0.1' },
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let output = '';
  relay.stdout.setEncoding('utf8');
  relay.stdout.on('data', (chunk) => { output += chunk; });
  let port;
  try {
    const started = await new Promise((resolve, reject) => {
      const timeout = setTimeout(() => reject(new Error('Relay did not start: ' + output)), 5000);
      relay.stdout.on('data', () => {
        const match = output.match(/listening on 127\.0\.0\.1:(\d+)/);
        if (match) {
          clearTimeout(timeout);
          resolve(Number(match[1]));
        }
      });
      relay.once('error', reject);
      relay.once('exit', (code) => reject(new Error('Relay exited before listening, code ' + code)));
    });
    port = started;

    const host = await connectClient(port);
    const guest = await connectClient(port);
    host.socket.write('HOST\tHost\n');
    const hosted = await waitForLine(host);
    const match = hosted.match(/^HOSTED\t([A-HJ-NP-Z2-9]{6})$/);
    assert.ok(match, 'host receives a six-character room code');

    guest.socket.write('JOIN\t' + match[1] + '\tGuest\n');
    const [joined, peerJoined] = await Promise.all([waitForLine(guest), waitForLine(host)]);
    assert.equal(joined, 'JOINED\t' + match[1] + '\tHost');
    assert.equal(peerJoined, 'PEER_JOINED\tGuest');

    guest.socket.write('INPUT\t1\t5\t2\t1\n');
    assert.equal(await waitForLine(host), 'INPUT\t1\t5\t2\t1');

    guest.socket.end();
    assert.equal(await waitForLine(host), 'PEER_LEFT');
    host.socket.end();
    process.stdout.write('PipeClean relay integration: PASS\n');
  } finally {
    relay.kill();
  }
}
main().catch((err) => {
  process.stderr.write('PipeClean relay integration: FAIL: ' + err.message + '\n');
  process.exitCode = 1;
});
