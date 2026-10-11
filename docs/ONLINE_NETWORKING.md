# PipeClean online co-op (experimental)

PipeClean has an experimental relay-based co-op path for **Super Mario Land** and
**Super Mario Land 2: 6 Golden Coins**. The relay pairs two players using an
eight-character invite code and forwards frame-numbered inputs. Both clients
must use the exact same game and ROM checksum.

This is still an early lockstep implementation. A full two-device gameplay test
has not been completed, so treat online gameplay as experimental.

## Relay connection modes

The Online page's Relay Address supports these forms:

- `127.0.0.1:27800` — plain TCP, for a relay on the same computer.
- `192.168.x.x:27800` (or another private LAN address) — plain TCP on a trusted
  home network, with the server explicitly bound to the LAN.
- `tls://relay-name.example:443` — TLS-encrypted Internet relay. The client
  validates the server certificate chain and hostname before sending its name,
  invite code, or gameplay messages. Use a hostname covered by a valid public
  certificate; raw public IP addresses are not accepted in plain-TCP mode.

The relay protocol itself remains plain TCP. For Internet play, terminate TLS
at a trusted edge proxy and forward traffic to the relay over the provider's
private network. Never expose port 27800 directly to the public Internet.

## Deploy a secure Internet relay with Fly.io

The repository includes `fly.toml` and `server/Dockerfile`. The Fly TLS
handler terminates client TLS at port 443 and forwards the decrypted relay
protocol to port 27800 inside Fly's private network. See the
[Fly TLS handler documentation](https://fly.io/docs/networking/services/).

1. Install the Fly CLI and sign in with `fly auth login`.
2. Choose a globally unique app name, then edit `app = "pipeclean-relay-change-this"`
   in `fly.toml` to that name.
3. From the repository root, create the app with `fly apps create YOUR-APP-NAME`.
4. Deploy with `fly deploy`.
5. Confirm the app is running with `fly status` and inspect any errors with
   `fly logs`.
6. In **both** PipeClean clients, set Relay Address to
   `tls://YOUR-APP-NAME.fly.dev:443`. Host on one client, share the invite
   code, then join on the second client.

The default `*.fly.dev` hostname uses Fly-managed TLS certificates. The
container listens on `0.0.0.0:27800` inside Fly; the public service is TLS
on port 443. Leave the internal port private. Do not add public port 27800,
and do not configure router port forwarding.

For a different hosting provider, configure an external TLS-terminating proxy
with a publicly trusted certificate, then forward TCP to the relay process on
port 27800 over a trusted private connection. Connect using
`tls://the-certificate-hostname:443`; the hostname must match the certificate.

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
- The relay creates unpredictable eight-character invite codes, limits
  connections/join attempts/message rates, expires unattended rooms, bounds
  messages, and allows only recognized protocol messages after pairing.
- TLS encrypts traffic in transit but this version has no account login,
  platform-level abuse monitoring, or matchmaking. Invite codes function as
  bearer credentials.
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
