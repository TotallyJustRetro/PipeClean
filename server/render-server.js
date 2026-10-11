#!/usr/bin/env node
'use strict';

/*
 * Render Web Service adapter.
 *
 * Render accepts HTTP/WebSocket connections, while PipeClean's existing room
 * relay intentionally speaks a small newline-delimited TCP protocol. This
 * process exposes /relay over WebSocket and bridges each text message to the
 * loopback-only relay. It also exposes /healthz for Render health checks.
 */
const http = require('node:http');
const net = require('node:net');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { WebSocket, WebSocketServer } = require('ws');

const WEB_PORT = Number.parseInt(process.env.PORT || '10000', 10);
const RELAY_PORT = Number.parseInt(process.env.PIPECLEAN_INTERNAL_RELAY_PORT || '27800', 10);
const HOST = '0.0.0.0';
const WS_PATH = '/relay';
const MAX_MESSAGE_BYTES = 128;
const MAX_RELAY_BUFFER = 2048;

if (!Number.isInteger(WEB_PORT) || WEB_PORT < 1 || WEB_PORT > 65535) {
  throw new Error('PORT must be a valid TCP port');
}
if (!Number.isInteger(RELAY_PORT) || RELAY_PORT < 1 || RELAY_PORT > 65535 || RELAY_PORT === WEB_PORT) {
  throw new Error('PIPECLEAN_INTERNAL_RELAY_PORT must be a valid port distinct from PORT');
}

const relay = spawn(process.execPath, [path.join(__dirname, 'relay.js')], {
  env: {
    ...process.env,
    PIPECLEAN_RELAY_BIND: '127.0.0.1',
    PIPECLEAN_RELAY_PORT: String(RELAY_PORT),
    PIPECLEAN_RELAY_PROXY_PROTOCOL: '1'
  },
  stdio: ['ignore', 'pipe', 'inherit']
});

let relayReady = false;
let stopping = false;
let relayStdoutBuffer = '';
const httpServer = http.createServer((request, response) => {
  const url = new URL(request.url || '/', 'http://localhost');
  if (request.method === 'GET' && url.pathname === '/healthz') {
    response.writeHead(relayReady && !stopping ? 200 : 503, {
      'content-type': 'application/json; charset=utf-8',
      'cache-control': 'no-store'
    });
    response.end(JSON.stringify({ ok: relayReady && !stopping, service: 'pipeclean-relay' }));
    return;
  }
  if (request.method === 'GET' && url.pathname === '/') {
    response.writeHead(200, { 'content-type': 'text/plain; charset=utf-8', 'cache-control': 'no-store' });
    response.end('PipeClean relay bridge is running. Connect through the PipeClean client.\n');
    return;
  }
  response.writeHead(404, { 'content-type': 'text/plain; charset=utf-8' });
  response.end('Not found\n');
});

const wss = new WebSocketServer({
  noServer: true,
  maxPayload: MAX_MESSAGE_BYTES,
  perMessageDeflate: false,
  clientTracking: true
});

httpServer.on('upgrade', (request, socket, head) => {
  let pathname;
  try {
    pathname = new URL(request.url || '/', 'http://localhost').pathname;
  } catch (_) {
    socket.destroy();
    return;
  }
  if (stopping || !relayReady || pathname !== WS_PATH) {
    socket.write('HTTP/1.1 503 Service Unavailable\r\nConnection: close\r\n\r\n');
    socket.destroy();
    return;
  }
  wss.handleUpgrade(request, socket, head, (websocket) => {
    wss.emit('connection', websocket, request);
  });
});

function closeSocket(ws, code, reason) {
  if (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING) {
    try { ws.close(code, reason); } catch (_) { ws.terminate(); }
  }
}

function clientAddress(request) {
  /* Render documents the first X-Forwarded-For address as the real client IP.
     Use it for relay rate limiting instead of Render's internal proxy address. */
  const forwarded = request.headers['x-forwarded-for'];
  if (typeof forwarded === 'string') {
    for (const item of forwarded.split(',')) {
      const candidate = item.trim().replace(/^::ffff:/, '');
      if (net.isIP(candidate)) return candidate;
    }
  }
  const remote = String(request.socket.remoteAddress || '').replace(/^::ffff:/, '');
  return net.isIP(remote) ? remote : '127.0.0.1';
}

function ipv6Bytes(address) {
  let value = address.toLowerCase().split('%')[0];
  const halves = value.split('::');
  if (halves.length > 2) return null;
  const left = halves[0] ? halves[0].split(':') : [];
  const right = halves.length === 2 && halves[1] ? halves[1].split(':') : [];
  const missing = 8 - left.length - right.length;
  if ((halves.length === 1 && missing !== 0) || (halves.length === 2 && missing < 1))
    return null;
  const groups = left.concat(Array(Math.max(0, missing)).fill('0'), right);
  if (groups.length !== 8 || groups.some((part) => !/^[0-9a-f]{1,4}$/.test(part)))
    return null;
  const output = Buffer.alloc(16);
  groups.forEach((part, index) => output.writeUInt16BE(parseInt(part, 16), index * 2));
  return output;
}

function makeProxyV2Header(address, relayPort) {
  const signature = Buffer.from([13, 10, 13, 10, 0, 13, 10, 81, 85, 73, 84, 10]);
  const versionCommand = 0x21;
  const family = net.isIP(address);
  if (family === 4) {
    const source = Buffer.from(address.split('.').map(Number));
    const addresses = Buffer.alloc(12);
    source.copy(addresses, 0);
    Buffer.from([127, 0, 0, 1]).copy(addresses, 4);
    addresses.writeUInt16BE(0, 8);
    addresses.writeUInt16BE(relayPort, 10);
    const header = Buffer.alloc(16);
    signature.copy(header);
    header[12] = versionCommand;
    header[13] = 0x11; // TCP over IPv4
    header.writeUInt16BE(addresses.length, 14);
    return Buffer.concat([header, addresses]);
  }
  const source = family === 6 ? ipv6Bytes(address) : null;
  if (source) {
    const addresses = Buffer.alloc(36);
    source.copy(addresses, 0);
    ipv6Bytes('::1').copy(addresses, 16);
    addresses.writeUInt16BE(0, 32);
    addresses.writeUInt16BE(relayPort, 34);
    const header = Buffer.alloc(16);
    signature.copy(header);
    header[12] = versionCommand;
    header[13] = 0x21; // TCP over IPv6
    header.writeUInt16BE(addresses.length, 14);
    return Buffer.concat([header, addresses]);
  }
  return makeProxyV2Header('127.0.0.1', relayPort);
}

wss.on('connection', (websocket, request) => {
  if (!relayReady || stopping) {
    closeSocket(websocket, 1013, 'Relay is starting');
    return;
  }
  const originalAddress = clientAddress(request);
  const tcp = net.createConnection({ host: '127.0.0.1', port: RELAY_PORT });
  let tcpConnected = false;
  let relayBuffer = '';
  let queuedBytes = 0;
  const queuedMessages = [];

  tcp.setNoDelay(true);
  tcp.on('connect', () => {
    /* The internal relay requires PROXY v2 so rate limits can use client IPs. */
    tcp.write(makeProxyV2Header(originalAddress, RELAY_PORT));
    tcpConnected = true;
    for (const message of queuedMessages) tcp.write(message);
    queuedMessages.length = 0;
    queuedBytes = 0;
  });

  websocket.on('message', (data, isBinary) => {
    if (isBinary) {
      closeSocket(websocket, 1003, 'Text messages only');
      tcp.destroy();
      return;
    }
    const message = data.toString('utf8');
    const bytes = Buffer.byteLength(message, 'utf8');
    /* The relay protocol has one bounded, newline-terminated command per WS frame. */
    if (bytes === 0 || bytes > MAX_MESSAGE_BYTES ||
        !message.endsWith('\n') ||
        message.slice(0, -1).includes('\n') ||
        message.includes('\0') ||
        message.includes('\r')) {
      closeSocket(websocket, 1008, 'Invalid relay message');
      tcp.destroy();
      return;
    }
    if (tcpConnected) {
      if (tcp.writableLength + bytes > MAX_RELAY_BUFFER) {
        closeSocket(websocket, 1013, 'Relay is busy');
        tcp.destroy();
        return;
      }
      tcp.write(message);
      return;
    }
    if (queuedBytes + bytes > MAX_RELAY_BUFFER) {
      closeSocket(websocket, 1013, 'Relay is busy');
      tcp.destroy();
      return;
    }
    queuedMessages.push(message);
    queuedBytes += bytes;
  });

  tcp.on('data', (chunk) => {
    relayBuffer += chunk.toString('utf8');
    let newline;
    while ((newline = relayBuffer.indexOf('\\n')) !== -1) {
      const line = relayBuffer.slice(0, newline).replace(/\\r$/, '');
      relayBuffer = relayBuffer.slice(newline + 1);
      const bytes = Buffer.byteLength(line, 'utf8');
      if (bytes > MAX_MESSAGE_BYTES) {
        closeSocket(websocket, 1011, 'Relay response too large');
        tcp.destroy();
        return;
      }
      if (websocket.readyState !== WebSocket.OPEN) return;
      if (websocket.bufferedAmount + bytes > MAX_RELAY_BUFFER) {
        closeSocket(websocket, 1013, 'Client is too slow');
        tcp.destroy();
        return;
      }
      websocket.send(line, { binary: false }, (error) => {
        if (error) tcp.destroy();
      });
    }
    /* Bound only the unterminated tail, not a valid burst of complete lines. */
    if (Buffer.byteLength(relayBuffer, 'utf8') > MAX_MESSAGE_BYTES) {
      closeSocket(websocket, 1011, 'Invalid relay response');
      tcp.destroy();
    }
  });

  tcp.on('error', () => closeSocket(websocket, 1011, 'Relay connection failed'));
  tcp.on('close', () => closeSocket(websocket, 1000, 'Relay disconnected'));
  websocket.on('close', () => tcp.destroy());
  websocket.on('error', () => tcp.destroy());
});

relay.stdout.setEncoding('utf8');
relay.stdout.on('data', (chunk) => {
  process.stdout.write('[relay] ' + chunk);
  relayStdoutBuffer += chunk;
  if (relayStdoutBuffer.length > 8192) relayStdoutBuffer = relayStdoutBuffer.slice(-4096);
  if (relayStdoutBuffer.includes('PipeClean relay listening on 127.0.0.1:' + RELAY_PORT)) {
    relayReady = true;
    if (!httpServer.listening) {
      httpServer.listen(WEB_PORT, HOST, () => {
        process.stdout.write('PipeClean Render WebSocket bridge listening on ' + HOST + ':' + WEB_PORT + '\\n');
      });
    }
  }
});

relay.on('error', (error) => {
  process.stderr.write('Could not start relay: ' + error.message + '\n');
  process.exitCode = 1;
});
relay.on('exit', (code, signal) => {
  relayReady = false;
  if (!stopping) {
    process.stderr.write('Relay process exited unexpectedly (code=' + code + ', signal=' + signal + ').\n');
    process.exitCode = 1;
    httpServer.close();
  }
});

function shutdown() {
  if (stopping) return;
  stopping = true;
  relayReady = false;
  for (const client of wss.clients) client.terminate();
  wss.close();
  httpServer.close(() => {
    if (relay.exitCode === null && relay.signalCode === null) relay.kill('SIGTERM');
  });
  const shutdownFallback = setTimeout(() => {
    if (relay.exitCode === null && relay.signalCode === null) relay.kill('SIGKILL');
  }, 5000);
  shutdownFallback.unref();
}
process.on('SIGTERM', shutdown);
process.on('SIGINT', shutdown);
