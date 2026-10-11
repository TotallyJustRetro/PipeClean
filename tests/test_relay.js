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
function makeProxyV2Header(sourceIp, destinationPort) {
  const signature = Buffer.from([13, 10, 13, 10, 0, 13, 10, 81, 85, 73, 84, 10]);
  const source = Buffer.from(sourceIp.split('.').map(Number));
  assert.equal(source.length, 4);
  const addresses = Buffer.alloc(12);
  source.copy(addresses, 0);
  Buffer.from([127, 0, 0, 1]).copy(addresses, 4);
  addresses.writeUInt16BE(40000, 8);
  addresses.writeUInt16BE(destinationPort, 10);
  const header = Buffer.alloc(16);
  signature.copy(header, 0);
  header[12] = 0x21; // PROXY command, version 2
  header[13] = 0x11; // TCP over IPv4
  header.writeUInt16BE(addresses.length, 14);
  return Buffer.concat([header, addresses]);
}
function connectClient(port, sourceIp = '198.51.100.10') {
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
    socket.once('connect', () => {
      socket.write(makeProxyV2Header(sourceIp, port));
      resolve(client);
    });
    socket.once('error', reject);
  });
}
async function main() {
  const relayEnv = {
    ...process.env,
    PIPECLEAN_RELAY_PORT: '0',
    PIPECLEAN_RELAY_PROXY_PROTOCOL: '1'
  };
  /* Verify the server's loopback-only default, even if the parent environment
     happens to set an override. */
  delete relayEnv.PIPECLEAN_RELAY_BIND;
  const relay = spawn(process.execPath, ['server/relay.js'], {
    env: relayEnv,
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

    const invalid = await connectClient(port, '198.51.100.20');
    invalid.socket.write('UNSUPPORTED\tcommand\n');
    assert.match(await waitForLine(invalid), /^ERR\tFirst message must be HOST or JOIN$/);
    invalid.socket.destroy();

    const host = await connectClient(port, '198.51.100.21');
    const guest = await connectClient(port, '198.51.100.22');
    host.socket.write('HOST\tHost\n');
    const hosted = await waitForLine(host);
    const match = hosted.match(/^HOSTED\t([A-HJ-NP-Z2-9]{8})$/);
    assert.ok(match, 'host receives an eight-character room code');

    guest.socket.write('JOIN\t' + match[1] + '\tGuest\n');
    const [joined, peerJoined] = await Promise.all([waitForLine(guest), waitForLine(host)]);
    assert.equal(joined, 'JOINED\t' + match[1] + '\tHost');
    assert.equal(peerJoined, 'PEER_JOINED\tGuest');

    guest.socket.write('INPUT\t1\t5\t2\t1\n');
    assert.equal(await waitForLine(host), 'INPUT\t1\t5\t2\t1');

    /* A TCP data event may include more than 2 KiB of complete, individually valid lines. */
    const firstFrame = 10000000000000000000n;
    const burst = Array.from({ length: 60 }, (_, i) =>
      `INPUT\t${firstFrame + BigInt(i)}\t255\t255\t255\n`).join('');
    assert.ok(Buffer.byteLength(burst) > 2048, 'burst exceeds the old aggregate-buffer limit');
    guest.socket.write(burst);
    const forwardedBurst = await Promise.all(Array.from({ length: 60 }, () => waitForLine(host)));
    assert.match(forwardedBurst[0], /^INPUT\t10000000000000000000\t255\t255\t255$/);
    assert.match(forwardedBurst[59], /^INPUT\t10000000000000000059\t255\t255\t255$/);


    guest.socket.write('UNSUPPORTED\tcommand\n');
    assert.equal(await waitForLine(guest), 'ERR\tUnsupported or malformed gameplay message');
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
