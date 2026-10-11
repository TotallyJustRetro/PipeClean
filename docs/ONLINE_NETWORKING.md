# PipeClean networking prototype

This adds the first real networking layer: a TCP relay, host/join handshakes,
six-character room codes, peer presence, and disconnect detection. The launcher
now connects to a relay instead of displaying placeholder success messages.

## Start a relay locally

Requires Node.js 18 or newer. From the repository root, run:

    node server/relay.js

By default it listens on TCP port 27800 on all interfaces. Override this with
PIPECLEAN_RELAY_PORT and PIPECLEAN_RELAY_BIND environment variables.

## Connect two PipeClean clients

1. Start the relay on a computer both clients can reach.
2. In each client's Online tab, set Relay address to that computer's hostname
   or IPv4 address and port, e.g. 192.168.1.20:27800.
3. On the host, click Host Session. The relay creates a six-character invite code.
4. Share that code with the other player. They enter it and click Join.
5. The Online panel reports when the second client connects. Disconnect closes
   the session cleanly.

For same-computer testing, use 127.0.0.1:27800. For another device on the same
home network, use the relay computer's LAN IPv4 address. Public internet access
requires a reachable relay server and firewall configuration.

## Important limitations

This is the transport/session milestone, not yet online gameplay. It does not
start a game or synchronize controller inputs / emulator frames yet. The relay
forwards later newline-delimited messages so frame-tagged input packets can be
added in the gameplay-sync phase.

The prototype protocol is plain TCP without TLS or authentication. Use it only
on a trusted LAN for now. Do not expose it to the public internet until encrypted
transport, authentication/rate limits, and session validation are added.


## Relay integration test

The regression workflow runs the relay in a temporary local port and verifies room
creation, joining, packet forwarding, and peer-disconnect notifications:

    node tests/test_relay.js
