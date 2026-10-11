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
- `tls://relay-name.example:443` — TLS-encrypted Internet relay. The client
  validates the server certificate chain and hostname before sending its name,
  invite code, or gameplay messages. Use a hostname covered by a valid public
  certificate; raw public IP addresses are not accepted in plain-TCP mode.

The relay protocol itself remains plain TCP. For Internet play, terminate TLS
at a trusted edge proxy and forward traffic to the relay over the provider's
private network. Never expose port 27800 directly to the public Internet.

## Deploy the official traversal relay from GitHub

The repository includes a manually triggered GitHub Actions workflow at
`.github/workflows/deploy-relay.yml`. Once the workflow is merged into the
repository's default branch, a maintainer can deploy the relay without installing
the Fly CLI locally.

One-time hosting setup:

1. Create or use a Fly.io account and choose the organization that will own the
   relay. Hosting is external infrastructure, but players will not need Fly
   accounts, terminal commands, or additional software.
2. Create an organization-scoped Fly API token and save it in the repository
   under **Settings → Secrets and variables → Actions** as
   `FLY_API_TOKEN`. The token is required because the workflow creates the Fly
   application on first deployment. Treat it like a password.
3. Open **Actions → Deploy PipeClean traversal relay → Run workflow**. Enter a
   globally unique application name and the Fly organization slug.

The workflow creates (or reuses) the app, deploys the Docker relay, checks its
status, writes the resulting `tls://APP_NAME.fly.dev:443` endpoint into
`runtime/settings.h`, commits that endpoint and the app name to the selected
branch, and triggers regression and Android builds. New installs then use that
TLS endpoint by default; existing settings still using the previous
`127.0.0.1:27800` default are migrated. User-configured relay addresses are
preserved.

The Fly TLS handler terminates client TLS on port 443 and forwards the relay
protocol to port 27800 inside Fly's private network. The configuration keeps
port 27800 private, enables a TCP health check, and keeps one machine running
because room codes are stored in process memory. Never expose port 27800
directly to the public Internet.

You can also deploy manually with the Fly CLI if preferred: set the `app` value
in `fly.toml` to your unique app name, create the app in your Fly organization,
then run `fly deploy`. After deployment, both clients must use
`tls://APP_NAME.fly.dev:443` until the resulting endpoint is compiled into the
default setting.

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
