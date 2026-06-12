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
* Designed and tested for the ESP32-ETH02 Ethernet module

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


## Hardware

All hardware files required to build the project are located in the `hardware` directory.

The utility power sensing adapter must be connected to a non-UPS outlet. If it is connected to a UPS-backed outlet, the system will be unable to detect a utility power failure.

### Included Files

* Bill of Materials (BOM)
* PCB design files
* Schematic files

Refer to the BOM documentation for a complete list of required components, sourcing information, and assembly notes.

The 3D printable enclosure files are located in the `enclosure` directory.

### Programming the ESP32-ETH02

The ESP32-ETH02 does not include a built-in USB programming interface.

To upload firmware, you will need one of the following:

* An ESP32-ETH02 programming adapter board
* A USB-to-Serial adapter with access to the required programming pins

The easiest option is to use one of the ESP32-ETH02 programming adapter boards available from Amazon, AliExpress, and other suppliers. These adapter boards typically provide:

* USB-C connection
* USB-to-Serial interface
* BOOT button
* RESET button
* Power regulation

These adapters allow the ESP32-ETH02 to be programmed directly from the Arduino IDE without additional wiring.



## Firmware Installation

The firmware has been developed and tested on the ESP32-ETH02 Ethernet module.

Other LAN8720-based ESP32 Ethernet boards may work, but they have not been tested with this project and are not currently supported.

Open the `CtrlAltDefib.ino` sketch located in:

```text
firmware/CtrlAltDefib/
```

and upload it to your ESP32-ETH02 using the Arduino IDE.

### Configure Web Interface Credentials

Before compiling and uploading the firmware, edit the following lines in `CtrlAltDefib.ino`:

```cpp
constexpr const char *UI_USERNAME = "admin";
constexpr const char *UI_PASSWORD = "CHANGE-ME";
```

Change the password to a secure value before uploading the firmware.

Example:

```cpp
constexpr const char *UI_USERNAME = "admin";
constexpr const char *UI_PASSWORD = "MySecurePassword";
```

The web interface will use these credentials for authentication.

### Arduino IDE Settings

The firmware was developed and tested using the following Arduino IDE settings:

| Setting                              | Value                                            |
| ------------------------------------ | ------------------------------------------------ |
| Board                                | ESP32 Dev Module                                 |
| CPU Frequency                        | 240MHz (WiFi/BT)                                 |
| Core Debug Level                     | Info                                             |
| Erase All Flash Before Sketch Upload | Disabled                                         |
| Events Run On                        | Core 1                                           |
| Flash Frequency                      | 40MHz                                            |
| Flash Mode                           | DIO                                              |
| Flash Size                           | 4MB (32Mb)                                       |
| JTAG Adapter                         | Disabled                                         |
| Arduino Runs On                      | Core 1                                           |
| Partition Scheme                     | Default 4MB with spiffs (1.2MB APP/1.5MB SPIFFS) |
| PSRAM                                | Disabled                                         |
| Upload Speed                         | 115200                                           |
| Zigbee Mode                          | Disabled                                         |

### Uploading the Firmware

1. Connect the ESP32-ETH02 to your programming adapter.
2. Connect the programming adapter to your computer using USB.
3. Open `firmware/CtrlAltDefib/CtrlAltDefib.ino` in the Arduino IDE.
4. Configure the username and password in the sketch before compiling.
5. Select the settings listed above.
6. Compile and upload the firmware.

### Required Libraries

Install any required libraries when prompted by the Arduino IDE.

After the firmware has been uploaded successfully, continue with the Proxmox Shutdown Service Setup section.

## Proxmox Shutdown Service Setup

The shutdown service runs inside a lightweight Debian LXC container and receives authenticated shutdown requests from the ESP32.

The installer automatically creates and configures the container, installs all required software, generates SSH keys, and configures the shutdown service.

### Prerequisites

* Proxmox VE 8.x or newer
* Internet access from the Proxmox host
* A Debian 12 container template available in Proxmox

If a Debian 12 template is not already installed:

1. Open the Proxmox web interface.
2. Select the local storage.
3. Open **CT Templates**.
4. Download the latest **Debian 12 Standard** template.

### Download the Repository

Log in to the Proxmox host as root and clone the repository:

```bash
git clone https://github.com/shaneaune/CtrlAltDefib.git
cd CtrlAltDefib/shutdown-service
```

### Run the Installer

Make the installer executable and start it:

```bash
chmod +x install-shutdown-service.sh
./install-shutdown-service.sh
```

The installer will:

* Create the shutdown-service container
* Install required packages
* Generate the SSH shutdown key
* Install and start the shutdown service
* Display the ESP32 configuration settings
* Generate the authorized_keys entry required for Proxmox shutdown access

### Complete the Action Required Step

When the installer completes, it will display a line similar to:

```text
command="shutdown -h now",no-port-forwarding,no-agent-forwarding,no-pty ssh-ed25519 ...
```

Copy the entire line and add it to:

```text
/root/.ssh/authorized_keys
```

on the Proxmox host.

You can retrieve the generated line again at any time with:

```bash
pct exec <CTID> -- cat /root/proxmox_authorized_key.txt
```

### Record the ESP32 Settings

The installer will display:

* Shutdown Service IP
* Shutdown Service Port
* Shutdown Token

These values will be required when configuring the ESP32 web interface.

### First Shutdown Test

After the ESP32 has been configured, perform a shutdown test.

The first shutdown request will automatically save the Proxmox host SSH key within the shutdown-service container. Future shutdown requests will use the saved key automatically.

Verify that:

* The ESP32 detects utility power loss
* The shutdown timer expires as expected
* The Proxmox host shuts down cleanly
* Wake-on-LAN startup functions correctly after power restoration

## ESP32 Configuration

After uploading the firmware and connecting the ESP32-ETH02 to your network, the device will automatically obtain an IP address using DHCP.

Locate the assigned IP address using your router, DHCP server, or a network scanning tool and open the web interface in your browser:

```text
http://<ip-address>/
```

### Log In

Log in using the username and password configured in the firmware before uploading.

After logging in, the Status page will be displayed. Open the **Configuration** page to access the system settings.

### Configure the Shutdown Service

On the Configuration page, enter the values provided by the shutdown-service installer:

* Shutdown Service IP Address
* Shutdown Service Port
* Shutdown Token

### Configure Shutdown Settings

Set the desired shutdown delay. This determines how long the system will wait after utility power is lost before initiating a Proxmox shutdown.

### Configure Wake-on-LAN

Enter the MAC address of the Proxmox host and configure the desired startup delay.

When utility power is restored, the ESP32 will wait for the configured startup delay before sending the Wake-on-LAN packet.

### Save Settings

Save the configuration using the web interface. The ESP32 will begin monitoring utility power and managing shutdown and startup events using the configured settings.

## First Shutdown Test

Before relying on Ctrl+Alt+Defib to protect your Proxmox host, perform a complete functional test.

### Verify Power Detection

1. Ensure the ESP32 is powered from a UPS-backed outlet.
2. Ensure the utility power sensing adapter is connected to a non-UPS outlet.
3. Open the Ctrl+Alt+Defib Status page.
4. Verify that utility power is reported as present.

### Verify Shutdown Operation

1. Temporarily disconnect the utility power sensing adapter.
2. Verify that the shutdown countdown begins.
3. Allow the countdown to expire.
4. Confirm that the Proxmox host shuts down cleanly.

### Verify Startup Operation

1. Restore utility power.
2. Verify that the startup countdown begins.
3. Allow the startup delay to expire.
4. Confirm that a Wake-on-LAN packet is sent.
5. Verify that the Proxmox host powers on successfully.

### Verify System Status

After the test is complete, confirm that:

* Utility power status is reported correctly.
* Shutdown countdown operates correctly.
* Startup countdown operates correctly.
* The Proxmox host shuts down cleanly.
* Wake-on-LAN startup functions correctly.
* Event logs show the expected activity.

Once these checks have been completed successfully, the system is ready for normal operation.

