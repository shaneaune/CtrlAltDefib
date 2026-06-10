#!/usr/bin/env bash
set -euo pipefail

echo "Ctrl+Alt+Defib shutdown-service installer"
echo

if ! command -v pct >/dev/null 2>&1; then
  echo "ERROR: pct command not found. Run this installer on a Proxmox host."
  exit 1
fi

if ! command -v pveversion >/dev/null 2>&1; then
  echo "ERROR: pveversion command not found. Run this installer on a Proxmox host."
  exit 1
fi

ask_yes_no() {
  local prompt="$1"
  local default_yes="${2:-true}"
  local answer

  if [[ "$default_yes" == "true" ]]; then
    read -rp "$prompt [Y/n]: " answer
    [[ -z "$answer" || "$answer" =~ ^[Yy]$ ]]
  else
    read -rp "$prompt [y/N]: " answer
    [[ "$answer" =~ ^[Yy]$ ]]
  fi
}

find_free_ctid() {
  local id=120

  while pct status "$id" >/dev/null 2>&1; do
    id=$((id + 1))
  done

  echo "$id"
}

echo "Proxmox detected:"
pveversion
echo

SUGGESTED_CTID="$(find_free_ctid)"

if ask_yes_no "Use automatically selected container ID $SUGGESTED_CTID?" true; then
  CTID="$SUGGESTED_CTID"
else
  read -rp "Enter container ID: " CTID
fi

if ask_yes_no "Use default hostname 'shutdown-service'?" true; then
  HOSTNAME="shutdown-service"
else
  read -rp "Enter container hostname: " HOSTNAME
fi

if ask_yes_no "Use default shutdown service port 8080?" true; then
  SERVICE_PORT="8080"
else
  read -rp "Enter shutdown service port: " SERVICE_PORT
fi

if [[ -z "$SERVICE_PORT" ]]; then
  echo "ERROR: Shutdown service port cannot be empty."
  exit 1
fi

if ask_yes_no "Use DHCP networking for the container?" true; then
  USE_DHCP="yes"
  CONTAINER_IP=""
  GATEWAY=""
else
  USE_DHCP="no"
  read -rp "Container IP/CIDR (example 10.0.0.117/24): " CONTAINER_IP
  read -rp "Gateway IP: " GATEWAY
fi

read -rp "Proxmox host IP address: " PROXMOX_HOST_IP

if [[ -z "$PROXMOX_HOST_IP" ]]; then
  echo "ERROR: Proxmox host IP is required."
  exit 1
fi

if ask_yes_no "Generate shutdown bearer token automatically?" true; then
  SHUTDOWN_TOKEN="$(openssl rand -hex 16)"
else
  read -rp "Enter shutdown bearer token: " SHUTDOWN_TOKEN
fi

if [[ -z "$SHUTDOWN_TOKEN" ]]; then
  echo "ERROR: Shutdown token cannot be empty."
  exit 1
fi

echo
echo "Selected settings:"
echo "  CT ID: $CTID"
echo "  Hostname: $HOSTNAME"
echo "  Service port: $SERVICE_PORT"
echo "  Networking: $([[ "$USE_DHCP" == "yes" ]] && echo "DHCP" || echo "Static $CONTAINER_IP via $GATEWAY")"
echo "  Proxmox host IP: $PROXMOX_HOST_IP"
echo "  Shutdown token: $SHUTDOWN_TOKEN"
echo
echo "Checking for Debian 12 LXC template..."

TEMPLATE="$(ls /var/lib/vz/template/cache/debian-12-standard*.tar.zst 2>/dev/null | sort | tail -n 1 || true)"

if [[ -z "$TEMPLATE" ]]; then
  echo "ERROR: No Debian 12 LXC template found."
  echo
  echo "Download one in Proxmox:"
  echo "  local -> CT Templates -> Templates -> debian-12-standard"
  exit 1
fi

echo "Using template:"
echo "  $TEMPLATE"
echo
echo "Creating container..."

if pct status "$CTID" >/dev/null 2>&1; then
  echo "ERROR: Container ID $CTID already exists."
  exit 1
fi

if [[ "$USE_DHCP" == "yes" ]]; then
  pct create "$CTID" "$TEMPLATE" \
    --hostname "$HOSTNAME" \
    --memory 256 \
    --cores 1 \
    --rootfs local-lvm:4 \
    --net0 name=eth0,bridge=vmbr0,ip=dhcp \
    --unprivileged 1
else
  pct create "$CTID" "$TEMPLATE" \
    --hostname "$HOSTNAME" \
    --memory 256 \
    --cores 1 \
    --rootfs local-lvm:4 \
    --net0 name=eth0,bridge=vmbr0,ip="$CONTAINER_IP",gw="$GATEWAY" \
    --unprivileged 1
fi

echo "Container created successfully."

echo
echo "Starting container..."
pct start "$CTID"

echo "Waiting for container to boot..."
sleep 8

echo "Container started."
echo
echo "Installing packages in container..."
pct exec "$CTID" -- apt update
pct exec "$CTID" -- apt install -y python3 openssh-client

echo "Packages installed."

echo
echo "Generating shutdown SSH key inside container..."

pct exec "$CTID" -- mkdir -p /root/.ssh

if pct exec "$CTID" -- test -f /root/.ssh/proxmox_shutdown_key; then
  echo "SSH key already exists, reusing existing key."
else
  pct exec "$CTID" -- ssh-keygen -t ed25519 -f /root/.ssh/proxmox_shutdown_key -N "" -C "root@shutdown-service"
fi

pct exec "$CTID" -- chmod 700 /root/.ssh
pct exec "$CTID" -- chmod 600 /root/.ssh/proxmox_shutdown_key
pct exec "$CTID" -- chmod 644 /root/.ssh/proxmox_shutdown_key.pub

echo "SSH key ready."

echo
echo "Installing shutdown service..."

pct exec "$CTID" -- mkdir -p /opt/ctrlaltdefib

pct exec "$CTID" -- bash -c "cat > /opt/ctrlaltdefib/shutdown_service.py" <<EOF
#!/usr/bin/env python3

from http.server import BaseHTTPRequestHandler, HTTPServer
import subprocess

TOKEN = "$SHUTDOWN_TOKEN"
PROXMOX_HOST = "$PROXMOX_HOST_IP"
SSH_KEY = "/root/.ssh/proxmox_shutdown_key"
PORT = $SERVICE_PORT

class ShutdownHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path != "/shutdown":
            self.send_response(404)
            self.end_headers()
            self.wfile.write(b"Not found")
            return

        auth = self.headers.get("Authorization", "")
        expected = "Bearer " + TOKEN

        if auth != expected:
            self.send_response(403)
            self.end_headers()
            self.wfile.write(b"Forbidden")
            return

        try:
            subprocess.Popen([
                "ssh",
                "-i", SSH_KEY,
                "-o", "StrictHostKeyChecking=accept-new",
                "root@" + PROXMOX_HOST
            ])

            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"Shutdown command sent")

        except Exception as e:
            self.send_response(500)
            self.end_headers()
            self.wfile.write(str(e).encode())

    def log_message(self, format, *args):
        return

if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", PORT), ShutdownHandler)
    server.serve_forever()
EOF

pct exec "$CTID" -- chmod +x /opt/ctrlaltdefib/shutdown_service.py

echo "Shutdown service script installed."

echo
echo "Installing systemd service..."

pct exec "$CTID" -- bash -c "cat > /etc/systemd/system/shutdown-service.service" <<EOF
[Unit]
Description=Ctrl+Alt+Defib Shutdown Service
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /opt/ctrlaltdefib/shutdown_service.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

pct exec "$CTID" -- systemctl daemon-reload
pct exec "$CTID" -- systemctl enable shutdown-service
pct exec "$CTID" -- systemctl start shutdown-service

echo "Systemd service installed and started."
echo
echo
echo "Verifying shutdown service..."

SERVICE_STATUS="$(pct exec "$CTID" -- systemctl is-active shutdown-service)"

if [[ "$SERVICE_STATUS" == "active" ]]; then
  echo "Shutdown service status: ACTIVE"
else
  echo "ERROR: Shutdown service failed to start."
  pct exec "$CTID" -- systemctl status shutdown-service --no-pager
  exit 1
fi

echo
echo "Detecting container IP address..."

CONTAINER_DETECTED_IP="$(pct exec "$CTID" -- hostname -I | awk '{print $1}')"

if [[ -z "$CONTAINER_DETECTED_IP" ]]; then
  echo "WARNING: Could not automatically detect container IP address."
  CONTAINER_DETECTED_IP="<container-ip>"
fi

PUBLIC_KEY="$(pct exec "$CTID" -- cat /root/.ssh/proxmox_shutdown_key.pub)"

echo
echo "######################################################################"
echo "# ACTION REQUIRED                                                    #"
echo "######################################################################"
echo
echo "Copy the following line into:"
echo
echo "  /root/.ssh/authorized_keys"
echo
echo "on the Proxmox host:"
echo
echo "---------------------------------------------------------------------"
echo "command=\"shutdown -h now\",no-port-forwarding,no-agent-forwarding,no-pty $PUBLIC_KEY"

AUTHORIZED_KEY_LINE="command=\"shutdown -h now\",no-port-forwarding,no-agent-forwarding,no-pty $PUBLIC_KEY"

pct exec "$CTID" -- bash -c "cat > /root/proxmox_authorized_key.txt" <<EOF
$AUTHORIZED_KEY_LINE
EOF

echo "---------------------------------------------------------------------"
echo
echo "The authorized_keys entry has also been saved to:"
echo
echo "  /root/proxmox_authorized_key.txt"
echo
echo "You can retrieve it later with:"
echo
echo "  pct exec $CTID -- cat /root/proxmox_authorized_key.txt"
echo
echo "Ctrl+Alt+Defib ESP32 settings:"
echo
echo "Shutdown service IP:   $CONTAINER_DETECTED_IP"
echo "Shutdown service port: $SERVICE_PORT"
echo "Shutdown token:        $SHUTDOWN_TOKEN"
echo
echo "WARNING: Any SSH connection using this restricted key will shut down the Proxmox host."
echo "The first shutdown request from the ESP32 will automatically save the Proxmox host key."
echo
echo "Installer completed successfully."
echo
echo "Complete the ACTION REQUIRED section above before testing."
