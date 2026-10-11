# PipeClean online co-op (experimental)

PipeClean now has a first experimental online co-op path for **Super Mario Land**
and **Super Mario Land 2: 6 Golden Coins**. The native client connects to a
small TCP relay, pairs two players with a six-character invite code, verifies
that both players selected the same game and ROM checksum, then exchanges
frame-numbered controller inputs so the existing shared-screen co-op simulation
can run on both clients.

This is an early lockstep implementation: it intentionally waits when the other
client is behind or disconnected. Online co-op has not yet been verified in a
full two-device gameplay test, and should be treated as experimental.

## Start the relay

Requires Node.js 18 or newer. From the repository root, run:

    node server/relay.js

The relay listens on TCP port 27800 by default. Override the listen address and
port with PIPECLEAN_RELAY_BIND and PIPECLEAN_RELAY_PORT environment variables.

## Connect and play

1. Start the relay on a computer both clients can reach.
2. In both PipeClean apps, open **Online** and set the same relay address.
   Use 127.0.0.1:27800 only when the relay is running on that same computer.
   For another PC or Android device on your home network, use the relay PC's LAN
   IPv4 address, for example 192.168.1.20:27800.
3. On one client, press **Host Session** and share the six-character invite code.
4. On the second client, enter that code and press **Join**.
5. Once the status says the other player is connected, both players select the
   exact same Super Mario Land or Super Mario Land 2 ROM from their Game tab and
   start it. The relay/game handshake checks both the game ID and ROM CRC.
6. The host controls Mario (Player 1); the joining client controls Luigi
   (Player 2). Both clients advance using the exchanged frame-numbered inputs.
   Disconnecting or leaving the game ends the online game session.

The current online path deliberately disables pause, turbo, rewind, and local
save-state loading while playing, because these can desynchronize the clients.
The online path avoids loading/writing the local battery save during that
session. Use identical ROM revisions and identical ROM-hack/patch results.

## Security warning

The relay currently uses **plain TCP with no TLS, account authentication, or
session authentication**. The invite code is only a room locator, not a security
credential. Use this prototype on a trusted local network only. Do not expose
it directly to the public internet yet. A production-ready public relay needs
encrypted transport, authentication, abuse/rate limiting, room/session
validation, and better resilience to high latency and packet loss.

## Relay integration test

The regression workflow starts the relay on a temporary local port and checks
room creation, joining, input-packet forwarding, and peer-disconnect notices:

    node tests/test_relay.js
