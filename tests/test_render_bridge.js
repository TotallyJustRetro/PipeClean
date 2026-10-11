'use strict';

const assert = require('node:assert/strict');
const net = require('node:net');
const { spawn } = require('node:child_process');
const path = require('node:path');
const WebSocket = require(require.resolve('ws', { paths: [path.join(__dirname, '../server')] }));

function reservePort() {
  return new Promise((resolve, reject) => {
    const server = net.createServer();
    server.once('error', reject);
    server.listen(0, '127.0.0.1', () => {
      const port = server.address().port;
      server.close((error) => error ? reject(error) : resolve(port));
    });
  });
}

function waitForMessage(ws, timeoutMs = 3000) {
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => {
      ws.off('message', onMessage);
      reject(new Error('Timed out waiting for WebSocket message'));
    }, timeoutMs);
    function onMessage(data, isBinary) {
      clearTimeout(timeout);
      resolve({ data: data.toString('utf8'), isBinary });
    }
    ws.once('message', onMessage);
  });
}

async function waitForHealth(url, child) {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    if (child.exitCode !== null) throw new Error('Render bridge exited before health check');
    try {
      const response = await fetch(url + '/healthz');
      if (response.status === 200) {
        const body = await response.json();
        if (body.ok === true) return;
      }
    } catch (_) {}
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error('Render bridge health check timed out');
}

async function main() {
  const webPort = await reservePort();
  const relayPort = await reservePort();
  if (webPort === relayPort) return main();

  const child = spawn(process.execPath, [path.join(__dirname, '../server/render-server.js')], {
    env: {
      ...process.env,
      PORT: String(webPort),
      PIPECLEAN_INTERNAL_RELAY_PORT: String(relayPort)
    },
    stdio: ['ignore', 'pipe', 'pipe']
  });
  let logs = '';
  child.stdout.setEncoding('utf8');
  child.stderr.setEncoding('utf8');
  child.stdout.on('data', (chunk) => { logs += chunk; });
  child.stderr.on('data', (chunk) => { logs += chunk; });

  const clients = [];
  try {
    await waitForHealth('http://127.0.0.1:' + webPort, child);
    const host = new WebSocket('ws://127.0.0.1:' + webPort + '/relay');
    clients.push(host);
    await new Promise((resolve, reject) => {
      host.once('open', resolve);
      host.once('error', reject);
    });
    host.send('HOST\tTest Host\n');
    const hosted = await waitForMessage(host);
    assert.equal(hosted.isBinary, false);
    const match = hosted.data.match(/^HOSTED\t([A-HJ-NP-Z2-9]{8})$/);
    assert.ok(match, 'Render WebSocket bridge passes host command to TCP relay');

    const guest = new WebSocket('ws://127.0.0.1:' + webPort + '/relay');
    clients.push(guest);
    await new Promise((resolve, reject) => {
      guest.once('open', resolve);
      guest.once('error', reject);
    });
    guest.send('JOIN\t' + match[1] + '\tTest Guest\n');
    const [joined, peerJoined] = await Promise.all([waitForMessage(guest), waitForMessage(host)]);
    assert.equal(joined.data, 'JOINED\t' + match[1] + '\tTest Host');
    assert.equal(peerJoined.data, 'PEER_JOINED\tTest Guest');

    guest.send('INPUT\t1\t5\t2\t1\n');
    assert.equal((await waitForMessage(host)).data, 'INPUT\t1\t5\t2\t1');

    const health = await fetch('http://127.0.0.1:' + webPort + '/healthz');
    assert.equal(health.status, 200);
    process.stdout.write('PipeClean Render WebSocket bridge integration: PASS\n');
  } finally {
    for (const client of clients) {
      try { client.terminate(); } catch (_) {}
    }
    child.kill('SIGTERM');
    await new Promise((resolve) => {
      if (child.exitCode !== null || child.signalCode !== null) return resolve();
      child.once('exit', resolve);
      setTimeout(resolve, 2000).unref();
    });
    if (child.exitCode !== 0 && child.exitCode !== null) {
      process.stderr.write(logs);
      throw new Error('Render bridge process failed with exit code ' + child.exitCode);
    }
  }
}

main().catch((error) => {
  process.stderr.write('PipeClean Render bridge integration: FAIL: ' + error.stack + '\n');
  process.exitCode = 1;
});
