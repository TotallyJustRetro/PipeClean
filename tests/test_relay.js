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

    host.socket.write('PING\n');
    assert.equal(await waitForLine(host), 'PONG');


    guest.socket.write('JOIN\t' + match[1] + '\tGuest\n');
    const [joined, peerJoined] = await Promise.all([waitForLine(guest), waitForLine(host)]);
    assert.equal(joined, 'JOINED\t' + match[1] + '\tHost');
    assert.equal(peerJoined, 'PEER_JOINED\tGuest');

    /* Host-owned game selection is echoed to both players. */
    host.socket.write('SELECT_GAME\t2\n');
    const [hostSelection, guestSelection] = await Promise.all([
      waitForLine(host), waitForLine(guest)
    ]);
    assert.equal(hostSelection, 'SELECT_GAME\t2');
    assert.equal(guestSelection, 'SELECT_GAME\t2');

    /* Both peers must submit the same local ROM checksum before the host can start. */
    host.socket.write('ROM_CHECK\t2\t1234ABCD\n');
    guest.socket.write('ROM_CHECK\t2\t1234ABCD\n');
    const [hostRomMatch, guestRomMatch] = await Promise.all([
      waitForLine(host), waitForLine(guest)
    ]);
    assert.equal(hostRomMatch, 'ROM_RESULT\t2\t1\t1234ABCD\t1\t1234ABCD');
    assert.equal(guestRomMatch, 'ROM_RESULT\t2\t1\t1234ABCD\t1\t1234ABCD');

    /* Chat is relayed to both players with the sender's room identity. */
    guest.socket.write('CHAT\thello from Android!\n');
    const [hostChat, guestChat] = await Promise.all([
      waitForLine(host), waitForLine(guest)
    ]);
    assert.equal(hostChat, 'CHAT\tGuest\thello from Android!');
    assert.equal(guestChat, 'CHAT\tGuest\thello from Android!');

    /* Only the host can initiate the shared game start. */
    host.socket.write('START_GAME\t2\n');
    const [hostStarted, guestStarted] = await Promise.all([
      waitForLine(host), waitForLine(guest)
    ]);
    assert.equal(hostStarted, 'START_GAME\t2');
    assert.equal(guestStarted, 'START_GAME\t2');

    /* A mismatching guest CRC must block the next start for both peers. */
    guest.socket.write('ROM_CHECK\t2\tDEADBEEF\n');
    const [hostRomMismatch, guestRomMismatch] = await Promise.all([
      waitForLine(host), waitForLine(guest)
    ]);
    assert.equal(hostRomMismatch, 'ROM_RESULT\t2\t1\t1234ABCD\t1\tDEADBEEF');
    assert.equal(guestRomMismatch, 'ROM_RESULT\t2\t1\t1234ABCD\t1\tDEADBEEF');
    host.socket.write('START_GAME\t2\n');
    assert.equal(await waitForLine(host), 'START_DENIED\tboth players must pass the ROM check');

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


    /* Named shared slots and host/guest authority checks. */
    host.socket.write('SAVE_NAME\t0\tMain Run\n');
    const [hostName, guestName] = await Promise.all([waitForLine(host), waitForLine(guest)]);
    assert.equal(hostName, 'SAVE_NAME\t0\tMain Run');
    assert.equal(guestName, 'SAVE_NAME\t0\tMain Run');

    guest.socket.write('SAVE_NAME\t0\tCheat Name\n');
    assert.match(await waitForLine(guest), /^ERR\tOnly the host can rename shared save slots$/);
    assert.equal(await waitForLine(host), 'PEER_LEFT');

    host.socket.write('SAVE_SELECT\t0\t-1\n');
    const [hostSelect, guestSelect] = await Promise.all([waitForLine(host), waitForLine(guest)]);
    assert.equal(hostSelect, 'SAVE_SELECT\t0\t-1');
    assert.equal(guestSelect, 'SAVE_SELECT\t0\t-1');
    guest.socket.write('SAVE_STATUS\t0\t1\n');
    assert.equal(await waitForLine(host), 'SAVE_STATUS\t0\t1');

    /* The relay preserves bounded base64 chunks used for authoritative state transfer. */
    host.socket.write('SAVE_BEGIN\t7\t0\t3\t352441C2\n');
    assert.equal(await waitForLine(guest), 'SAVE_BEGIN\t7\t0\t3\t352441C2');
    host.socket.write('SAVE_CHUNK\t7\t0\t0\tQUJD\n');
    assert.equal(await waitForLine(guest), 'SAVE_CHUNK\t7\t0\t0\tQUJD');
    host.socket.write('SAVE_END\t7\t0\t3\t352441C2\n');
    assert.equal(await waitForLine(guest), 'SAVE_END\t7\t0\t3\t352441C2');
    guest.socket.write('SAVE_ACK\t7\t0\t352441C2\n');
    assert.equal(await waitForLine(host), 'SAVE_ACK\t7\t0\t352441C2');
    guest.socket.write('SAVE_FAIL\t8\t0\n');
    assert.equal(await waitForLine(host), 'SAVE_FAIL\t8\t0');

    /* Guests cannot change the host-selected game. */
    guest.socket.write('SELECT_GAME\t1\n');
    assert.equal(await waitForLine(guest), 'ERR\tOnly the host can choose the game');
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
