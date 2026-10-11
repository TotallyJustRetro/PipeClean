#!/usr/bin/env bash
set -Eeuo pipefail

# Run as root on an Ubuntu 24.04 Oracle Cloud VM.
# Arguments: public TLS hostname and contact email for Let's Encrypt.
if [[ $# -ne 2 ]]; then
  echo "Usage: $0 <relay-hostname> <contact-email>" >&2
  exit 2
fi

RELAY_HOST="$1"
ACME_EMAIL="$2"
if [[ ! "$RELAY_HOST" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]{0,251}[A-Za-z0-9])?$ ]]; then
  echo "Invalid relay hostname." >&2
  exit 2
fi
if [[ ! "$ACME_EMAIL" =~ ^[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,63}$ ]]; then
  echo "A valid contact email is required for certificate renewal." >&2
  exit 2
fi
if [[ "$(id -u)" -ne 0 ]]; then
  echo "Run this script through sudo." >&2
  exit 2
fi
if [[ ! -f /etc/os-release ]] || ! grep -q '^ID=ubuntu$' /etc/os-release; then
  echo "This automated installer currently supports Ubuntu Server images only." >&2
  exit 2
fi
if [[ ! -s /tmp/pipeclean-relay.js ]]; then
  echo "Missing uploaded relay file at /tmp/pipeclean-relay.js." >&2
  exit 2
fi

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y nodejs nginx-full libnginx-mod-stream certbot openssl

# Run the room relay as an unprivileged local service. Nginx terminates TLS
# and forwards a PROXY v2 header over loopback so the relay can rate-limit by
# the original client address.
if ! id pipeclean >/dev/null 2>&1; then
  useradd --system --home-dir /opt/pipeclean-relay --shell /usr/sbin/nologin pipeclean
fi
install -d -o root -g pipeclean -m 0750 /opt/pipeclean-relay
install -o root -g pipeclean -m 0640 /tmp/pipeclean-relay.js /opt/pipeclean-relay/relay.js
node --check /opt/pipeclean-relay/relay.js

cat > /etc/systemd/system/pipeclean-relay.service <<'UNIT'
[Unit]
Description=PipeClean online traversal relay
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=pipeclean
Group=pipeclean
WorkingDirectory=/opt/pipeclean-relay
Environment=NODE_ENV=production
Environment=PIPECLEAN_RELAY_BIND=127.0.0.1
Environment=PIPECLEAN_RELAY_PORT=27800
Environment=PIPECLEAN_RELAY_PROXY_PROTOCOL=1
ExecStart=/usr/bin/node /opt/pipeclean-relay/relay.js
Restart=on-failure
RestartSec=2
NoNewPrivileges=true
PrivateTmp=true
ProtectSystem=strict
ProtectHome=true
ProtectKernelTunables=true
ProtectKernelModules=true
ProtectControlGroups=true
LockPersonality=true
RestrictSUIDSGID=true
UMask=0027

[Install]
WantedBy=multi-user.target
UNIT

install -d -o root -g root -m 0755 /var/www/certbot/.well-known/acme-challenge

# First start Nginx with HTTP-01 challenge support only, so Certbot can obtain
# a trusted certificate before the TLS stream listener is enabled.
cat > /etc/nginx/nginx.conf <<'NGINX_HTTP'
include /etc/nginx/modules-enabled/*.conf;
user www-data;
worker_processes auto;
pid /run/nginx.pid;
error_log /var/log/nginx/error.log;
events {
    worker_connections 1024;
}
http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;
    server_tokens off;
    server {
        listen 80 default_server;
        server_name _;
        location ^~ /.well-known/acme-challenge/ {
            root /var/www/certbot;
            default_type text/plain;
            try_files $uri =404;
        }
        location / {
            return 404;
        }
    }
}
NGINX_HTTP

nginx -t
systemctl enable --now nginx
systemctl reload nginx

certbot certonly \
  --webroot --webroot-path /var/www/certbot \
  --domain "$RELAY_HOST" \
  --email "$ACME_EMAIL" \
  --agree-tos --non-interactive --keep-until-expiring --no-eff-email

# The relay protocol uses raw TCP inside the VM; TLS is terminated at Nginx.
# Keep the Node relay bound to loopback and never expose port 27800 publicly.
cat > /etc/nginx/nginx.conf <<NGINX_FINAL
include /etc/nginx/modules-enabled/*.conf;
user www-data;
worker_processes auto;
pid /run/nginx.pid;
error_log /var/log/nginx/error.log;
events {
    worker_connections 1024;
}
http {
    include /etc/nginx/mime.types;
    default_type application/octet-stream;
    server_tokens off;
    server {
        listen 80 default_server;
        server_name $RELAY_HOST;
        location ^~ /.well-known/acme-challenge/ {
            root /var/www/certbot;
            default_type text/plain;
            try_files \$uri =404;
        }
        location / {
            return 404;
        }
    }
}
stream {
    server {
        listen 443 ssl;
        ssl_certificate /etc/letsencrypt/live/$RELAY_HOST/fullchain.pem;
        ssl_certificate_key /etc/letsencrypt/live/$RELAY_HOST/privkey.pem;
        ssl_protocols TLSv1.2 TLSv1.3;
        ssl_session_cache shared:PipeCleanTLS:1m;
        ssl_session_timeout 10m;
        proxy_connect_timeout 5s;
        proxy_timeout 20m;
        proxy_protocol on;
        proxy_pass 127.0.0.1:27800;
    }
}
NGINX_FINAL

cat > /etc/letsencrypt/renewal-hooks/deploy/reload-nginx.sh <<'HOOK'
#!/usr/bin/env bash
set -Eeuo pipefail
/usr/sbin/nginx -t
/bin/systemctl reload nginx
HOOK
chmod 0755 /etc/letsencrypt/renewal-hooks/deploy/reload-nginx.sh

nginx -t
systemctl enable --now pipeclean-relay.service
systemctl reload nginx
systemctl enable --now certbot.timer

# Basic state checks. Public reachability still depends on the OCI subnet/NSG
# allowing inbound TCP 80 and 443.
systemctl is-active --quiet pipeclean-relay.service
systemctl is-active --quiet nginx
ss -ltn | grep -q ':443 '
rm -f /tmp/pipeclean-relay.js /tmp/pipeclean-oracle-bootstrap.sh
echo "PipeClean relay is running behind Nginx TLS at $RELAY_HOST:443"
echo "Only ports 80 and 443 should be public; Node remains on 127.0.0.1:27800."
