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
const MAX_LINE = 12288;
const MAX_BUFFER = 32768;
const MAX_CONNECTIONS_PER_IP_PER_MINUTE = 24;
const MAX_JOINS_PER_IP_PER_MINUTE = 10;
const MAX_MESSAGES_PER_SECOND = 240;
const WAITING_ROOM_TTL_MS = 15 * 60 * 1000;
/* Waiting rooms are advertised as valid for 15 minutes. Keep idle relay
   sockets alive slightly longer so the expiry sweep can report an expired
   invite instead of silently dropping its host at the same instant. */
const SOCKET_IDLE_TIMEOUT_MS = Math.max(120_000, WAITING_ROOM_TTL_MS + 60_000);
const ROOM_SWEEP_INTERVAL_MS = Math.min(30_000, Math.max(1_000, Math.floor(WAITING_ROOM_TTL_MS / 4)));
const USE_PROXY_PROTOCOL = process.env.PIPECLEAN_RELAY_PROXY_PROTOCOL === '1';
const PROXY_V2_SIGNATURE = Buffer.from([13, 10, 13, 10, 0, 13, 10, 81, 85, 73, 84, 10]);
const MAX_PROXY_HEADER_BYTES = 512;
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
function parseProxyV2Header(buffer) {
  if (buffer.length < 16) return { pending: true };
  if (!buffer.subarray(0, 12).equals(PROXY_V2_SIGNATURE)) return { error: true };
  /* Only accept a version-2 PROXY command with a TCP/IPv4 or TCP/IPv6 source. */
  if (buffer[12] !== 0x21) return { error: true };
  const familyProtocol = buffer[13];
  const addressLength = buffer.readUInt16BE(14);
  const headerLength = 16 + addressLength;
  if (headerLength > MAX_PROXY_HEADER_BYTES) return { error: true };
  if (buffer.length < headerLength) return { pending: true };

  if (familyProtocol === 0x11) {
    if (addressLength < 12) return { error: true };
    const sourceIp = Array.from(buffer.subarray(16, 20)).join('.');
    return { sourceIp, headerLength };
  }
  if (familyProtocol === 0x21) {
    if (addressLength < 36) return { error: true };
    /* Binary-form IPv6 key stays stable without requiring textual compression. */
    const sourceIp = 'proxy6:' + buffer.subarray(16, 32).toString('hex');
    return { sourceIp, headerLength };
  }
  return { error: true };
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
    room.hostRom = null;
    room.guestRom = null;
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

function sendRomResult(room) {
  if (!room || !room.selectedGame || !room.hostRom || !room.guestRom) return;
  const host = room.hostRom;
  const guest = room.guestRom;
  if (host.game !== room.selectedGame || guest.game !== room.selectedGame) return;
  const hostCrc = host.ok ? host.crc : '00000000';
  const guestCrc = guest.ok ? guest.crc : '00000000';
  const line = 'ROM_RESULT\t' + room.selectedGame + '\t' +
    (host.ok ? '1' : '0') + '\t' + hostCrc + '\t' +
    (guest.ok ? '1' : '0') + '\t' + guestCrc;
  reply(room.host, line);
  reply(room.guest, line);
}
function processLine(client, line) {
  if (line.length > MAX_LINE) {
    rejectClient(client, 'Message too large');
    return;
  }
  /* Application heartbeat: keeps hosted WebSocket relays active while idle. */
  if (line === 'PING') {
    reply(client, 'PONG');
    return;
  }

  if (client.phase === 'new') {
    const parts = line.split('\t');
    if (parts[0] === 'HOST' && parts.length <= 2) {
      const room = {
        code: makeCode(), host: client, guest: null, selectedGame: null,
        hostRom: null, guestRom: null,
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
  const room = client.room;
  if (/^SELECT_GAME\t[12]$/.test(line)) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can choose the game');
      return;
    }
    const selectedGame = Number(line.slice('SELECT_GAME\t'.length));
    if (room.selectedGame !== selectedGame) {
      room.selectedGame = selectedGame;
      if (room.hostRom && room.hostRom.game !== selectedGame) room.hostRom = null;
      if (room.guestRom && room.guestRom.game !== selectedGame) room.guestRom = null;
    }
    reply(client, line);
    reply(client.peer, line);
    sendRomResult(room);
    return;
  }
  if (/^START_GAME\t[12]$/.test(line)) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can start the game');
      return;
    }
    const startGame = Number(line.slice('START_GAME\t'.length));
    if (room.selectedGame !== startGame) {
      reply(client, 'START_DENIED\tchoose a game in the lobby first');
      return;
    }
    const host = room.hostRom, guest = room.guestRom;
    if (!host || !guest || !host.ok || !guest.ok ||
        host.game !== startGame || guest.game !== startGame ||
        host.crc !== guest.crc) {
      reply(client, 'START_DENIED\tboth players must pass the ROM check');
      return;
    }
    reply(client, line);
    reply(client.peer, line);
    return;
  }
  if (line.startsWith('CHAT\t')) {
    const message = line.slice(5);
    if (!message || message.length > 88 || /[\t\r\n\u0000-\u001f\u007f]/.test(message)) {
      rejectClient(client, 'Invalid chat message');
      return;
    }
    const chatLine = 'CHAT\t' + client.name + '\t' + message;
    reply(client, chatLine);
    reply(client.peer, chatLine);
    return;
  }
  const romMatch = /^ROM_CHECK\t([12])\t([0-9A-Fa-f]{8})$/.exec(line);
  const romInvalid = /^ROM_INVALID\t([12])\t([1-9])$/.exec(line);
  if (romMatch || romInvalid) {
    const parsed = romMatch || romInvalid;
    const report = {
      game: Number(parsed[1]),
      ok: !!romMatch,
      crc: romMatch ? parsed[2].toUpperCase() : '00000000'
    };
    if (client === room.host) room.hostRom = report;
    else if (client === room.guest) room.guestRom = report;
    sendRomResult(room);
    return;
  }
  const saveName = /^SAVE_NAME\t([0-9])\t([A-Za-z0-9 _-]{1,31})$/.exec(line);
  if (saveName) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can rename shared save slots');
      return;
    }
    reply(client, line);
    reply(client.peer, line);
    return;
  }
  const saveSelect = /^SAVE_SELECT\t([0-9])\t(-1|[0-9])$/.exec(line);
  if (saveSelect) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can select the shared save slot');
      return;
    }
    reply(client, line);
    reply(client.peer, line);
    return;
  }
  const saveStatus = /^SAVE_STATUS\t([0-9])\t([01])$/.exec(line);
  if (saveStatus) {
    reply(client.peer, line);
    return;
  }
  const saveBegin = /^SAVE_BEGIN\t([1-9][0-9]{0,9})\t([0-9])\t([1-9][0-9]{0,6})\t([0-9A-Fa-f]{8})$/.exec(line);
  if (saveBegin) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can transfer a shared save');
      return;
    }
    if (Number(saveBegin[3]) > 1000000) {
      rejectClient(client, 'Shared save is too large');
      return;
    }
    reply(client.peer, line);
    return;
  }
  const saveChunk = /^SAVE_CHUNK\t([1-9][0-9]{0,9})\t([0-9])\t([0-9]{1,7})\t([A-Za-z0-9+/]+={0,2})$/.exec(line);
  if (saveChunk) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can transfer a shared save');
      return;
    }
    if (saveChunk[4].length > 10668 || saveChunk[4].length % 4 !== 0 ||
        Number(saveChunk[3]) > 1000000) {
      rejectClient(client, 'Invalid shared save chunk');
      return;
    }
    reply(client.peer, line);
    return;
  }
  const saveEnd = /^SAVE_END\t([1-9][0-9]{0,9})\t([0-9])\t([1-9][0-9]{0,6})\t([0-9A-Fa-f]{8})$/.exec(line);
  if (saveEnd) {
    if (client !== room.host) {
      rejectClient(client, 'Only the host can finish a shared save transfer');
      return;
    }
    reply(client.peer, line);
    return;
  }
  const saveAck = /^SAVE_ACK\t([1-9][0-9]{0,9})\t([0-9])\t([0-9A-Fa-f]{8})$/.exec(line);
  if (saveAck) {
    if (client !== room.guest) {
      rejectClient(client, 'Only the guest can acknowledge a shared save');
      return;
    }
    reply(client.peer, line);
    return;
  }
  const saveFail = /^SAVE_FAIL\t([1-9][0-9]{0,9})\t([0-9])$/.exec(line);
  if (saveFail) {
    if (client !== room.guest) {
      rejectClient(client, 'Only the guest can reject a shared save transfer');
      return;
    }
    reply(client.peer, line);
    return;
  }
  if (line === 'GAME_END' || validGameLine(line) || validInputLine(line)) {
    reply(client.peer, line);
    return;
  }
  rejectClient(client, 'Unsupported or malformed gameplay message');
}

const server = net.createServer((socket) => {
  const transportIp = (socket.remoteAddress || 'unknown').replace(/^::ffff:/, '');
  if (clientCount >= MAX_CONNECTIONS ||
      (!USE_PROXY_PROTOCOL &&
       !allowAttempt(connectAttempts, transportIp, MAX_CONNECTIONS_PER_IP_PER_MINUTE))) {
    socket.destroy();
    return;
  }
  clientCount += 1;
  socket.setNoDelay(true);
  socket.setKeepAlive(true, 10_000);
  /* A proxy-prefixed connection must send its header promptly. */
  socket.setTimeout(USE_PROXY_PROTOCOL ? 10_000 : SOCKET_IDLE_TIMEOUT_MS, () => socket.destroy());
  const client = {
    socket, ip: transportIp, phase: 'new', name: 'Player', room: null, peer: null,
    buffer: '', closed: false, rateStarted: Date.now(), rateCount: 0,
    proxyResolved: !USE_PROXY_PROTOCOL, proxyBuffer: Buffer.alloc(0)
  };
  socket.on('data', (rawChunk) => {
    let chunk = rawChunk;
    if (!client.proxyResolved) {
      const combined = Buffer.concat([client.proxyBuffer, rawChunk]);
      if (combined.length > MAX_PROXY_HEADER_BYTES + 16) { socket.destroy(); return; }
      const parsed = parseProxyV2Header(combined);
      if (parsed.error) { socket.destroy(); return; }
      if (parsed.pending) {
        client.proxyBuffer = combined;
        return;
      }
      client.ip = parsed.sourceIp;
      client.proxyResolved = true;
      client.proxyBuffer = Buffer.alloc(0);
      if (!allowAttempt(connectAttempts, client.ip, MAX_CONNECTIONS_PER_IP_PER_MINUTE)) {
        socket.destroy();
        return;
      }
      socket.setTimeout(SOCKET_IDLE_TIMEOUT_MS, () => socket.destroy());
      chunk = combined.subarray(parsed.headerLength);
      if (!chunk.length) return;
    }
    client.buffer += chunk.toString('utf8');
    /* Parse complete lines before bounding the remaining partial line; a single TCP chunk can contain many valid messages. */
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
  process.stdout.write(USE_PROXY_PROTOCOL
    ? 'Security mode: TLS must terminate at a trusted edge proxy; PROXY v2 required. Relay process itself has no TLS.\n'
    : 'Security mode: loopback default; no TLS. Trusted LAN only when explicitly exposed.\n');
});
