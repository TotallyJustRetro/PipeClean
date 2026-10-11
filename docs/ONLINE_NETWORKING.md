# PipeClean online co-op (experimental)

PipeClean now has a first experimental online co-op path for **Super Mario Land**
and **Super Mario Land 2: 6 Golden Coins**. The native client connects to a
small TCP relay, pairs two players with a eight-character invite code, verifies
that both players selected the same game and ROM checksum, then exchanges
frame-numbered controller inputs so the existing shared-screen co-op simulation
can run on both clients.

This is an early lockstep implementation: it intentionally waits when the other
client is behind or disconnected. Online co-op has not yet been verified in a
full two-device gameplay test, and should be treated as experimental.

## Start the relay

Requires Node.js 18 or newer. From the repository root, run:

    node server/relay.js

The relay listens on TCP port 27800 by default and binds to **127.0.0.1 only**.
That is deliberately safe-by-default: only clients running on the same computer
can connect. To make it reachable from another device on a trusted home LAN,
explicitly bind to LAN interfaces:

    PIPECLEAN_RELAY_BIND=0.0.0.0 PIPECLEAN_RELAY_PORT=27800 node server/relay.js

On Windows PowerShell, set the variables first with `$env:PIPECLEAN_RELAY_BIND = "0.0.0.0"`
and `$env:PIPECLEAN_RELAY_PORT = "27800"`, then run `node server/relay.js`.
Use the relay computer's LAN IPv4 address in both apps. Allow TCP port 27800 only
on the private network in the host firewall. Never configure router port
forwarding for this prototype.

## Connect and play

1. Start the relay on a computer both clients can reach.
2. In both PipeClean apps, open **Online** and set the same relay address.
   Use 127.0.0.1:27800 only when the relay is running on that same computer.
   For another PC or Android device on your home network, use the relay PC's LAN
   IPv4 address, for example 192.168.1.20:27800.
3. On one client, press **Host Session** and share the eight-character invite code.
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

The relay currently uses **plain TCP with no TLS or account authentication**.
It now binds to loopback by default, limits connections and invite attempts,
expires unattended rooms, and only forwards recognized gameplay protocol
messages with bounded sizes and message rates. Those measures reduce accidental
exposure and basic abuse, but they do **not** encrypt traffic or make invite
codes private. On a LAN, other devices on that network may be able to observe
traffic depending on the network. Do not expose this prototype directly to the
public internet. Before public online play, PipeClean still needs a TLS-capable
native transport on both Windows and Android, proper room/session credentials,
production hosting and monitoring, and two-device gameplay validation.

## Launcher display options

The global **Display** tab controls launcher UI size (75–125%) on desktop and
Android. The launcher switches to a compact two-row header navigation at small
effective UI scales, replacing the permanent desktop sidebar so the game and
settings pages have more usable width. Desktop users can also select Compact,
Balanced, or Large launcher windows and toggle launcher fullscreen. Android
leaves window sizing and fullscreen to the OS; per-game display settings remain
available under each game's own Display page.

## Relay integration test

The regression workflow starts the relay on a temporary local port and checks
room creation, joining, input-packet forwarding, and peer-disconnect notices:

    node tests/test_relay.js
