#!/usr/bin/env node
'use strict';

/*
 * PipeClean experimental room relay.
 *
 * Safe-by-default: bind to loopback. For a trusted LAN session, explicitly set
 * PIPECLEAN_RELAY_BIND=0.0.0.0. This relay does NOT provide TLS or authentication
 * and must not be exposed directly to the public internet.
 */
const net = require('node:net');
const crypto = require('node:crypto');

const PORT = Number.parseInt(process.env.PIPECLEAN_RELAY_PORT || '27800', 10);
const HOST = process.env.PIPECLEAN_RELAY_BIND || '127.0.0.1';
const ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const MAX_CONNECTIONS = 256;
const MAX_LINE = 128;
const MAX_BUFFER = 2048;
const MAX_CONNECTIONS_PER_IP_PER_MINUTE = 24;
const MAX_JOINS_PER_IP_PER_MINUTE = 10;
const MAX_MESSAGES_PER_SECOND = 240;
const WAITING_ROOM_TTL_MS = 15 * 60 * 1000;
/* Waiting rooms are advertised as valid for 15 minutes. Keep idle relay
   sockets alive for at least that long too; a 2-minute socket timeout used to
   disconnect hosts long before their invite codes expired. */
const SOCKET_IDLE_TIMEOUT_MS = Math.max(120_000, WAITING_ROOM_TTL_MS);
const ROOM_SWEEP_INTERVAL_MS = Math.min(30_000, Math.max(1_000, Math.floor(WAITING_ROOM_TTL_MS / 4)));
const rooms = new Map();
const connectAttempts = new Map();
const joinAttempts = new Map();
let clientCount = 0;

function allowAttempt(table, key, max, now = Date.now()) {
  const recent = (table.get(key) || []).filter((t) => now - t < 60_000);
  if (recent.length >= max) {
    table.set(key, recent);
    return false;
  }
  recent.push(now);
  table.set(key, recent);
  return true;
}
function makeCode() {
  for (let tries = 0; tries < 100; tries += 1) {
    let code = '';
    const bytes = crypto.randomBytes(12);
    for (let i = 0; i < 8; i += 1) code += ALPHABET[bytes[i] % ALPHABET.length];
    if (!rooms.has(code)) return code;
  }
  throw new Error('Could not allocate an invite code');
}
function safeName(value) {
  return String(value || 'Player')
    .replace(/[\u0000-\u001f\u007f\t]/g, '')
    .slice(0, 24) || 'Player';
}
function reply(client, line) {
  if (client && !client.closed && !client.socket.destroyed) {
    if (!client.socket.write(line + '\n')) client.socket.pause();
  }
}
function forgetRoom(room) {
  if (!room) return;
  if (rooms.get(room.code) === room) rooms.delete(room.code);
  room.expiresAt = 0;
}
function removeClient(client) {
  if (client.closed) return;
  client.closed = true;
  clientCount = Math.max(0, clientCount - 1);
  const room = client.room;
  if (!room) return;
  if (room.host === client) {
    forgetRoom(room);
    const guest = room.guest;
    if (guest && !guest.closed) {
      guest.room = null;
      guest.peer = null;
      reply(guest, 'PEER_LEFT');
      guest.socket.end();
    }
  } else if (room.guest === client) {
    room.guest = null;
    client.peer = null;
    room.expiresAt = Date.now() + WAITING_ROOM_TTL_MS;
    if (room.host && !room.host.closed) {
      room.host.peer = null;
      reply(room.host, 'PEER_LEFT');
    }
  }
  client.room = null;
  client.peer = null;
}
function rejectClient(client, message) {
  reply(client, 'ERR\t' + message);
  client.socket.end();
}
function validGameLine(line) {
  return /^GAME\t\d{1,3}\t[0-9A-Fa-f]{8}$/.test(line);
}
function validInputLine(line) {
  const match = /^INPUT\t(\d{1,20})\t(\d{1,3})\t(\d{1,3})\t(\d{1,3})$/.exec(line);
  if (!match) return false;
  try {
    if (BigInt(match[1]) > 18446744073709551615n) return false;
  } catch (_) { return false; }
  return match.slice(2).every((v) => Number(v) <= 255);
}
function processLine(client, line) {
  if (line.length > MAX_LINE) {
    rejectClient(client, 'Message too large');
    return;
  }
  if (client.phase === 'new') {
    const parts = line.split('\t');
    if (parts[0] === 'HOST' && parts.length <= 2) {
      const room = {
        code: makeCode(), host: client, guest: null,
        expiresAt: Date.now() + WAITING_ROOM_TTL_MS
      };
      client.room = room;
      client.phase = 'host';
      client.name = safeName(parts[1]);
      rooms.set(room.code, room);
      reply(client, 'HOSTED\t' + room.code);
      return;
    }
    if (parts[0] === 'JOIN' && parts.length === 3) {
      const code = String(parts[1] || '').toUpperCase();
      const ip = client.ip;
      if (!allowAttempt(joinAttempts, ip, MAX_JOINS_PER_IP_PER_MINUTE)) {
        rejectClient(client, 'Too many join attempts; try again later');
        return;
      }
      const room = rooms.get(code);
      if (!/^[A-HJ-NP-Z2-9]{8}$/.test(code) || !room ||
          (room.expiresAt && Date.now() > room.expiresAt) || room.guest) {
        if (room && room.expiresAt && Date.now() > room.expiresAt) forgetRoom(room);
        rejectClient(client, 'Invite code not found or room is full');
        return;
      }
      client.name = safeName(parts[2]);
      client.room = room;
      client.phase = 'paired';
      client.peer = room.host;
      room.guest = client;
      room.expiresAt = 0;
      room.host.peer = client;
      room.host.phase = 'paired';
      reply(client, 'JOINED\t' + room.code + '\t' + room.host.name);
      reply(room.host, 'PEER_JOINED\t' + client.name);
      return;
    }
    rejectClient(client, 'First message must be HOST or JOIN');
    return;
  }

  if (client.phase !== 'paired' || !client.peer || client.peer.closed) {
    rejectClient(client, 'Join a room before sending gameplay messages');
    return;
  }
  if (line === 'GAME_END' || validGameLine(line) || validInputLine(line)) {
    reply(client.peer, line);
    return;
  }
  rejectClient(client, 'Unsupported or malformed gameplay message');
}

const server = net.createServer((socket) => {
  const ip = (socket.remoteAddress || 'unknown').replace(/^::ffff:/, '');
  if (clientCount >= MAX_CONNECTIONS ||
      !allowAttempt(connectAttempts, ip, MAX_CONNECTIONS_PER_IP_PER_MINUTE)) {
    socket.destroy();
    return;
  }
  clientCount += 1;
  socket.setNoDelay(true);
  socket.setKeepAlive(true, 10_000);
  socket.setTimeout(SOCKET_IDLE_TIMEOUT_MS, () => socket.destroy());
  const client = {
    socket, ip, phase: 'new', name: 'Player', room: null, peer: null,
    buffer: '', closed: false, rateStarted: Date.now(), rateCount: 0
  };
  socket.on('data', (chunk) => {
    client.buffer += chunk.toString('utf8');
    if (client.buffer.length > MAX_BUFFER) { socket.destroy(); return; }
    let newline;
    while ((newline = client.buffer.indexOf('\n')) !== -1) {
      let line = client.buffer.slice(0, newline).replace(/\r$/, '');
      client.buffer = client.buffer.slice(newline + 1);
      const now = Date.now();
      if (now - client.rateStarted >= 1000) {
        client.rateStarted = now;
        client.rateCount = 0;
      }
      if (++client.rateCount > MAX_MESSAGES_PER_SECOND) {
        rejectClient(client, 'Message rate limit exceeded');
        break;
      }
      if (line.length > MAX_LINE) {
        rejectClient(client, 'Message too large');
        break;
      }
      processLine(client, line);
      if (client.closed || socket.destroyed || socket.writableEnded) break;
    }
    if (client.buffer.length > MAX_LINE) socket.destroy();
  });
  socket.on('drain', () => socket.resume());
  socket.on('close', () => removeClient(client));
  socket.on('error', (err) => {
    if (process.env.PIPECLEAN_RELAY_DEBUG)
      process.stderr.write('Client socket error: ' + err.message + '\n');
    removeClient(client);
  });
});
server.on('error', (err) => {
  process.stderr.write('PipeClean relay failed: ' + err.message + '\n');
  process.exitCode = 1;
});
const roomExpiry = setInterval(() => {
  const now = Date.now();
  for (const room of rooms.values()) {
    if (!room.guest && room.expiresAt && now > room.expiresAt) {
      const host = room.host;
      forgetRoom(room);
      if (host && !host.closed) {
        host.room = null;
        reply(host, 'ERR\tInvite expired; create a new room');
        host.socket.end();
      }
    }
  }
  for (const table of [connectAttempts, joinAttempts]) {
    for (const [key, times] of table) {
      const recent = times.filter((t) => now - t < 60_000);
      if (recent.length) table.set(key, recent);
      else table.delete(key);
    }
  }
}, ROOM_SWEEP_INTERVAL_MS);
roomExpiry.unref();
server.listen(PORT, HOST, () => {
  const address = server.address();
  const boundPort = address && typeof address === 'object' ? address.port : PORT;
  process.stdout.write('PipeClean relay listening on ' + HOST + ':' + boundPort + '\n');
  process.stdout.write('Security mode: loopback default; no TLS. Trusted LAN only when explicitly exposed.\n');
});
