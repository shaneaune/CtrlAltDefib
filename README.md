# Ctrl+Alt+Defib

**Ctrl+Alt+Defib** is an ESP32-based power outage monitoring and automated shutdown system designed for Proxmox home labs and small servers.

The system monitors a UPS-backed power signal and, when utility power is lost, starts a configurable shutdown timer. If power is not restored before the timer expires, the ESP32 sends a secure shutdown request to a Proxmox shutdown service running in a lightweight Linux container. The shutdown service then performs a controlled shutdown of the Proxmox host to protect virtual machines, containers, and storage from unexpected power loss.

The project is designed to be simple to deploy, inexpensive to build, and easy to configure. All settings are managed through a built-in web interface hosted directly on the ESP32.

---

## Features

* ESP32 Ethernet-based monitoring and control
* Configurable shutdown delay timer
* Web-based configuration interface
* Secure token-protected shutdown requests
* Automated Proxmox shutdown-service installer
* Wake-on-LAN support for automatic server startup after power restoration
* Local event logging and status monitoring
* Open-source hardware and firmware
* Designed for WT32-ETH01 and WT32-ETH02 modules

---

## Repository Structure

```text
firmware/
└── CtrlAltDefib/
    └── CtrlAltDefib.ino

hardware/
├── BOM/
├── PCB/
└── Schematic/

enclosure/
├── STL/
└── Printing Notes/

shutdown-service/
├── install-shutdown-service.sh
└── README.md
```

---

## How It Works

1. The ESP32 monitors the wall power.
2. If utility power is lost, a configurable shutdown countdown begins.
3. If power returns before the timer expires, the countdown is cancelled.
4. If the timer expires, the ESP32 sends a secure shutdown request to the Proxmox shutdown service.
5. The shutdown service securely shuts down the Proxmox host.
6. When power is restored, the ESP32 can optionally send a Wake-on-LAN packet to restart the server.

---

## Documentation

* Hardware Requirements
* Hardware Assembly
* Firmware Installation
* Proxmox Shutdown Service Setup
* ESP32 Configuration
* First Shutdown Test
* Troubleshooting

```
```
