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

## Deploy the official traversal relay from GitHub (Oracle Cloud)

The repository includes a manually triggered workflow at
`.github/workflows/deploy-relay-oracle.yml`. It uses an Oracle Cloud VM and
GitHub Actions to install/update the relay; you do not need to install the OCI
CLI, Node.js, Nginx, or deployment tooling on your own computer. The workflow
must be merged into the repository's default branch before it can be launched
from **Actions → Deploy PipeClean traversal relay to Oracle Cloud → Run workflow**.
When running it, select the branch you want the workflow to configure.

### One-time Oracle Cloud setup

1. Create an Oracle Cloud account and choose your home region carefully. Oracle
   generally asks for phone verification and a credit card; according to Oracle,
   the card is not charged unless you upgrade the account. Do **not** upgrade to
   a paid account for this setup.
2. In the OCI Console, create a VM using an **Always Free-eligible** shape in
   your home region. Use Ubuntu Server 24.04 (Arm/aarch64 works with the Node
   relay). Keep total Ampere A1 allocation at or below **2 OCPUs and 12 GB RAM**
   so it stays within the Always Free compute allowance. If available, a small
   configuration such as 1 OCPU and 6 GB RAM is sufficient to begin testing.
3. Create the VM in a public subnet and assign it a public IPv4 address. During
   VM creation, choose **Generate a key pair for me**, download the private key,
   and keep it private. Paste its complete contents into the GitHub Actions
   secret named `OCI_SSH_PRIVATE_KEY`.
4. In **Settings → Secrets and variables → Actions**, add the following:
   - Secret `OCI_RELAY_HOST`: the VM's public IPv4 address.
   - Secret `OCI_SSH_PRIVATE_KEY`: the downloaded private SSH key.
   - Variable `OCI_RELAY_USER`: `ubuntu` for the standard Ubuntu image.
   - Variable `PIPECLEAN_RELAY_EMAIL`: an email address for TLS certificate
     issuance and renewal.
   - Optional variable `PIPECLEAN_RELAY_HOSTNAME`: your own DNS hostname if
     you have one and have pointed its A record at the VM. Leave it unset to
     derive a hostname from the public IP using sslip.io.
5. In the VM's subnet Security List or Network Security Group, allow inbound
   TCP **22** for SSH and TCP **80** and **443** for the relay's certificate
   validation and TLS connections. Restrict SSH source addresses where practical.
   Do **not** open TCP port 27800 publicly; the relay stays bound to loopback.
6. Open **Actions → Deploy PipeClean traversal relay to Oracle Cloud → Run
   workflow**. Select your branch and start the workflow.

The workflow uploads the relay source over SSH, installs Node.js/Nginx/Certbot,
configures an unprivileged systemd service, obtains a public TLS certificate,
and enables certificate renewal. Nginx terminates TLS on port 443 and forwards
traffic plus a PROXY v2 header to the relay on loopback port 27800. It commits
the resulting `tls://HOSTNAME:443` endpoint as the default in
`runtime/settings.h` and triggers regression and Android builds.

### Free-tier limitations

Oracle's qualifying Always Free resources have no scheduled expiry, but they
are not unlimited and availability is not guaranteed. The documented compute
allowance is up to 2 OCPUs and 12 GB RAM for Ampere A1, with 10 TB/month of
outbound data transfer. Oracle can reclaim Always Free compute instances that
meet its low-utilization criteria for seven days; a quiet relay may therefore
not be guaranteed to remain available indefinitely. Free capacity can also be
temporarily unavailable in some regions. Review OCI's current Always Free
rules before relying on this as a permanent public service.

For users, the goal is still simple: download PipeClean, enter an invite code,
and play. Players do not need Oracle accounts or additional networking software.
The hosting owner does need an Oracle account and a one-time VM setup; the public
relay is not live until these steps complete and the workflow succeeds.

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
