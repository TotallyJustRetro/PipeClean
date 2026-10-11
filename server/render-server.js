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
    PIPECLEAN_RELAY_PROXY_PROTOCOL: '0'
  },
  stdio: ['ignore', 'pipe', 'inherit']
});

let relayReady = false;
let stopping = false;
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

wss.on('connection', (websocket) => {
  if (!relayReady || stopping) {
    closeSocket(websocket, 1013, 'Relay is starting');
    return;
  }
  const tcp = net.createConnection({ host: '127.0.0.1', port: RELAY_PORT });
  let tcpConnected = false;
  let relayBuffer = '';
  let queuedBytes = 0;
  const queuedMessages = [];

  tcp.setNoDelay(true);
  tcp.on('connect', () => {
    tcpConnected = true;
    for (const message of queuedMessages) {
      if (!tcp.write(message)) break;
    }
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
    if (relayBuffer.length > MAX_RELAY_BUFFER) {
      closeSocket(websocket, 1011, 'Invalid relay response');
      tcp.destroy();
      return;
    }
    let newline;
    while ((newline = relayBuffer.indexOf('\n')) !== -1) {
      const line = relayBuffer.slice(0, newline).replace(/\r$/, '');
      relayBuffer = relayBuffer.slice(newline + 1);
      if (websocket.readyState !== WebSocket.OPEN) return;
      if (Buffer.byteLength(line, 'utf8') > MAX_MESSAGE_BYTES) {
        closeSocket(websocket, 1011, 'Relay response too large');
        tcp.destroy();
        return;
      }
      websocket.send(line, { binary: false }, (error) => {
        if (error) tcp.destroy();
      });
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
  if (chunk.includes('PipeClean relay listening on 127.0.0.1:' + RELAY_PORT)) {
    relayReady = true;
    if (!httpServer.listening) {
      httpServer.listen(WEB_PORT, HOST, () => {
        process.stdout.write('PipeClean Render WebSocket bridge listening on ' + HOST + ':' + WEB_PORT + '\n');
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
