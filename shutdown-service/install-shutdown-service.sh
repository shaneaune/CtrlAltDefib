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

echo "Proxmox detected:"
pveversion
echo

read -rp "Container ID [120]: " CTID
CTID="${CTID:-120}"

read -rp "Container hostname [shutdown-service]: " HOSTNAME
HOSTNAME="${HOSTNAME:-shutdown-service}"

read -rp "Shutdown service port [8080]: " SERVICE_PORT
SERVICE_PORT="${SERVICE_PORT:-8080}"

read -rp "Proxmox host IP address: " PROXMOX_HOST_IP

if [[ -z "$PROXMOX_HOST_IP" ]]; then
  echo "ERROR: Proxmox host IP is required."
  exit 1
fi

read -rp "Shutdown bearer token [auto-generate]: " SHUTDOWN_TOKEN

if [[ -z "$SHUTDOWN_TOKEN" ]]; then
  SHUTDOWN_TOKEN="$(openssl rand -hex 16)"
fi

echo
echo "Selected settings:"
echo "  CT ID: $CTID"
echo "  Hostname: $HOSTNAME"
echo "  Service port: $SERVICE_PORT"
echo "  Proxmox host IP: $PROXMOX_HOST_IP"
echo "  Shutdown token: $SHUTDOWN_TOKEN"
echo
echo "Preflight complete."
