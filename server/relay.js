#!/usr/bin/env node
'use strict';

/*
 * PipeClean session relay prototype.
 * Pairs two clients, assigns short room codes, and forwards later text lines.
 * Gameplay input synchronization is deliberately a separate runtime phase.
 */
const net = require('node:net');
const crypto = require('node:crypto');

const PORT = Number.parseInt(process.env.PIPECLEAN_RELAY_PORT || '27800', 10);
const HOST = process.env.PIPECLEAN_RELAY_BIND || '0.0.0.0';
const ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789';
const rooms = new Map();

function makeCode() {
  for (let tries = 0; tries < 100; tries += 1) {
    let code = '';
    const bytes = crypto.randomBytes(6);
    for (let i = 0; i < 6; i += 1) code += ALPHABET[bytes[i] % ALPHABET.length];
    if (!rooms.has(code)) return code;
  }
  throw new Error('Could not allocate an invite code');
}
function safeName(value) {
  return String(value || 'Player').replace(/[\u0000-\u001f\u007f\t]/g, '').slice(0, 24) || 'Player';
}
function reply(client, line) {
  if (client && !client.closed && !client.socket.destroyed) client.socket.write(line + '\n');
}
function removeClient(client) {
  if (client.closed) return;
  client.closed = true;
  const room = client.room;
  if (!room) return;
  if (room.host === client) {
    rooms.delete(room.code);
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
    if (room.host && !room.host.closed) {
      room.host.peer = null;
      reply(room.host, 'PEER_LEFT');
    }
  }
}
function processLine(client, line) {
  if (line.length > 1024) {
    reply(client, 'ERR Message too large');
    client.socket.destroy();
    return;
  }
  if (client.phase === 'new') {
    const parts = line.split('\t');
    if (parts[0] === 'HOST') {
      const room = { code: makeCode(), host: client, guest: null };
      client.room = room;
      client.phase = 'host';
      client.name = safeName(parts.slice(1).join(' '));
      rooms.set(room.code, room);
      reply(client, 'HOSTED\t' + room.code);
      return;
    }
    if (parts[0] === 'JOIN') {
      const code = String(parts[1] || '').toUpperCase();
      const room = rooms.get(code);
      if (!/^[A-HJ-NP-Z2-9]{6}$/.test(code) || !room || room.guest) {
        reply(client, 'ERR Invite code not found or room is full');
        client.socket.end();
        return;
      }
      client.name = safeName(parts.slice(2).join(' '));
      client.room = room;
      client.phase = 'paired';
      client.peer = room.host;
      room.guest = client;
      room.host.peer = client;
      reply(client, 'JOINED\t' + room.code + '\t' + room.host.name);
      reply(room.host, 'PEER_JOINED\t' + client.name);
      return;
    }
    reply(client, 'ERR First message must be HOST or JOIN');
    client.socket.end();
    return;
  }
  /* Future INPUT / STATE lines are relayed unchanged between paired clients. */
  if (client.peer && !client.peer.closed) reply(client.peer, line);
  else if (client.phase === 'paired') reply(client, 'ERR Peer is no longer connected');
}
const server = net.createServer((socket) => {
  socket.setNoDelay(true);
  socket.setKeepAlive(true, 10000);
  socket.setTimeout(120000, () => socket.destroy());
  const client = {
    socket, phase: 'new', name: 'Player', room: null, peer: null,
    buffer: '', closed: false
  };
  socket.on('data', (chunk) => {
    client.buffer += chunk.toString('utf8');
    if (client.buffer.length > 4096) { socket.destroy(); return; }
    let newline;
    while ((newline = client.buffer.indexOf('\n')) !== -1) {
      const line = client.buffer.slice(0, newline).replace(/\r$/, '');
      client.buffer = client.buffer.slice(newline + 1);
      processLine(client, line);
      if (client.closed || socket.destroyed) break;
    }
  });
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
server.listen(PORT, HOST, () => {
  const address = server.address();
  const boundPort = address && typeof address === 'object' ? address.port : PORT;
  process.stdout.write('PipeClean relay listening on ' + HOST + ':' + boundPort + '\n');
  process.stdout.write('Prototype relay: trusted networks only; no TLS or authentication.\n');
});
