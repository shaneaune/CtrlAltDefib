# Ctrl+Alt+Defib

**Ctrl+Alt+Defib** is an ESP32-based power outage monitoring and automated shutdown system designed for Proxmox home labs and small servers.

The system monitors utility power using a standard 5V USB-C power adapter. When utility power is lost, a configurable shutdown timer begins. If power is not restored before the timer expires, the ESP32 sends a secure shutdown request to a Proxmox shutdown service running in a lightweight Linux container. The shutdown service then performs a controlled shutdown of the Proxmox host to protect virtual machines, containers, and storage from unexpected power loss.

The ESP32 remains powered by the UPS during an outage, allowing it to continue monitoring power status. When utility power is restored, a separate configurable startup timer begins. Once the startup delay has expired, the ESP32 can automatically send a Wake-on-LAN packet to restart the Proxmox host.

The project is designed to be simple to deploy, inexpensive to build, and easy to configure. All settings are managed through a built-in web interface hosted directly on the ESP32.

---

## Features

* ESP32 Ethernet-based monitoring and control
* Configurable shutdown delay timer
* Web-based configuration interface
* Secure token-protected shutdown requests
* Automated Proxmox shutdown-service installer
* Wake-on-LAN support for automatic Proxmox host startup after power restoration
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

1. The ESP32 is powered by the UPS so it remains operational during a power outage.
2. A standard 5V USB-C power adapter monitors the presence of utility power.
3. If utility power is lost, a configurable shutdown countdown begins.
4. If utility power returns before the timer expires, the shutdown countdown is cancelled.
5. If the timer expires, the ESP32 sends a secure shutdown request to the Proxmox shutdown service.
6. The shutdown service securely shuts down the Proxmox host.
7. When utility power is restored, a configurable startup countdown begins.
8. When the startup countdown expires, the ESP32 can automatically send a Wake-on-LAN packet to restart the Proxmox host.

---

## Documentation

* Hardware Requirements
* Hardware Assembly
* Firmware Installation
* Proxmox Shutdown Service Setup
* ESP32 Configuration
* First Shutdown Test
* Troubleshooting

## Hardware

All hardware files required to build the project are located in the `hardware` directory.

### Included Files

* Bill of Materials (BOM)
* PCB design files
* Schematic files

Refer to the BOM documentation for a complete list of required components, sourcing information, and assembly notes.

The 3D printable enclosure files are located in the `enclosure` directory.

* License

```
```
