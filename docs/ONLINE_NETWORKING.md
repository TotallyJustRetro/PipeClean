# PipeClean online co-op (experimental)

PipeClean has an experimental relay-based co-op path for **Super Mario Land** and
**Super Mario Land 2: 6 Golden Coins**. The relay pairs two players using an
eight-character invite code and forwards frame-numbered inputs. Both clients
must use the exact same game and ROM checksum.

This is still an early lockstep implementation. A full two-device gameplay test
has not been completed, so treat online gameplay as experimental.

## Connection mode selector

The Online page now has a saved dropdown with **Direct Connection** and **Traversal Server**. **Traversal Server** is the currently supported mode and uses the relay address and invite-code flow documented below. **Direct Connection** is surfaced in the UI but is not yet implemented; selecting it displays an explanation and Host/Join will not attempt a misleading relay connection. Direct mode requires a peer listener/connection handshake and handling for addresses behind NAT, which are separate networking work.

## Traversal relay address

The Online page's server address supports these forms:

- `127.0.0.1:27800` — plain TCP, for a relay on the same computer.
- `192.168.x.x:27800` (or another private LAN address) — plain TCP on a trusted
  home network, with the server explicitly bound to the LAN.
- `tls://relay-name.example:443` — legacy TLS-wrapped TCP for a dedicated TCP relay.
- `wss://your-service.onrender.com` — secure WebSocket connection for the Render-hosted relay. The client validates the TLS certificate and hostname, upgrades to WebSocket at `/relay`, and then exchanges the existing line protocol inside WebSocket text frames.

For public Render hosting, use `wss://` rather than `tls://`. Render Web Services accept HTTP/WebSocket traffic, not arbitrary inbound raw TCP. The public bridge listens on Render's `PORT` and forwards to the existing TCP relay on loopback port 27800, which must not be exposed separately.

## Deploy the traversal relay on Render (Free)

The repository now includes a Render-compatible WebSocket bridge in
`server/render-server.js`. The bridge starts the existing TCP relay as a
loopback-only child process and forwards WebSocket messages to it. Render
installs the `ws` dependency using `server/package.json`.

### Render dashboard settings

1. Create a **Web Service** from the `TotallyJustRetro/PipeClean` GitHub repository.
2. Choose branch `feature/online-android-foundation` while this experimental work remains in PR #8.
3. Set **Root Directory** to `server`.
4. Use build command `npm install --omit=dev`.
5. Use start command `npm start`.
6. Select the **Free** instance. Do not choose a paid compute type.
7. Add a health check path of `/healthz` if the dashboard offers that setting.
8. Deploy only after the branch contains the WebSocket client and bridge updates.

Do not set `PORT` manually; Render supplies it. The bridge starts the internal
relay on loopback port 27800 (or the optional
`PIPECLEAN_INTERNAL_RELAY_PORT` override for tests). The only public service
endpoint is the Render HTTPS/WebSocket URL.

### Configure PipeClean to use the deployed service

After Render reports the service URL, for example
`https://your-service.onrender.com`, the secure relay endpoint is
`wss://your-service.onrender.com`. The app uses port 443 by default when a
WSS URL omits a port. We can set that URL as the default in
`runtime/settings.h` and trigger the Windows/Android builds after deployment
succeeds.

### Free-tier behavior and limits

Render's Free web services may spin down after a period without traffic and
take time to wake up on the next request. Free services also have monthly
instance-hour and bandwidth limits; exceeding allowances can suspend service
until the quota resets. This is a no-card/free-hosting target, not unlimited or
guaranteed always-on hosting. Expect the first connection after idle to take
longer while Render starts the service.

For players, the intended flow remains: download PipeClean, host a session,
share the invite code, and connect from Windows or Android. Players do not need
Render accounts or networking tools. The account owner must authorize the one-
time Render setup, and real Windows-to-Android gameplay still needs testing.

## Run a local or trusted-LAN relay

Requires Node.js 18 or newer. From the repository root, run:

```sh
node server/relay.js
```

It listens on TCP `127.0.0.1:27800` by default, so only clients on the same
computer can connect.

For a trusted LAN session, explicitly set the bind address. POSIX shell:

```sh
PIPECLEAN_RELAY_BIND=0.0.0.0 PIPECLEAN_RELAY_PORT=27800 node server/relay.js
```

Windows PowerShell:

```powershell
$env:PIPECLEAN_RELAY_BIND = "0.0.0.0"
$env:PIPECLEAN_RELAY_PORT = "27800"
node server/relay.js
```

Use the relay computer's private LAN IPv4 address in both apps and allow TCP
port 27800 only on the private network in the host firewall. Plain TCP is not
encrypted; do not use it over untrusted networks or the public Internet.

## Connect and play

1. Start or deploy a relay reachable by both clients.
2. Enter the same Relay Address on both clients.
3. Press **Host Session** on one client and share the eight-character code.
4. Enter the code on the other client and press **Join**.
5. Both players select the exact same Super Mario Land or Super Mario Land 2
   ROM and start it. The game handshake checks both the game ID and ROM CRC.
6. The host controls Mario (Player 1); the joining client controls Luigi
   (Player 2). Both clients advance using exchanged frame-numbered inputs.

The online path intentionally disables pause, turbo, rewind, and local save-state
loading during play because these can desynchronize clients. It avoids local
battery-save loading/writing during the online session. Use identical ROM
revisions and identical ROM-hack/patch results.

## Security and current limits

- Plain TCP is restricted to loopback/private network destinations.
- `tls://` uses mbedTLS certificate-chain and hostname verification.
- Oracle deployments terminate TLS with Nginx and forward PROXY protocol v2 to
  the local relay. The Node relay binds to `127.0.0.1:27800`; only ports 80
  and 443 need public ingress (plus SSH for deployment).
- WSS clients send masked WebSocket ping/pong control frames and an application-level `PING`/`PONG` heartbeat during idle periods. The application heartbeat also counts as incoming WebSocket traffic for Render Free's idle timer and is not forwarded to the other player.
- The relay creates unpredictable eight-character invite codes, rate-limits
  using original peer addresses when available, expires unattended rooms after
  15 minutes, keeps idle sockets open long enough for that expiry to take effect,
  bounds messages, and allows only recognized protocol messages after pairing.
- TLS encrypts traffic in transit, but this version has no account login,
  platform-level abuse monitoring, or matchmaking. Invite codes function as
  bearer credentials.
- Oracle Always Free resources still have capacity, compute, bandwidth, and
  idle-reclamation limitations. The service is not a guarantee of uninterrupted
  public availability or unlimited traffic.
- Internet relay deployment and the TLS client build must pass CI, and
  Windows-to-Android gameplay still requires a real two-device test.

## Relay integration test

The regression workflow tests room creation, joining, input forwarding, malformed
messages, and disconnect handling:

```sh
node --check server/relay.js
node tests/test_relay.js
```

## Launcher display options

The global **Display** tab controls launcher UI size (75–125%) on desktop and
Android. Smaller effective layouts switch to a compact two-row header
navigation. Desktop users can select Compact, Balanced, or Large launcher
windows and toggle launcher fullscreen. Android leaves window sizing and
fullscreen to the OS.
