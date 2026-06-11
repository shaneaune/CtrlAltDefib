# Ctrl+Alt+Defib Bill of Materials

## Hardware

| Qty | Item                                 | Notes                                                         |
| --- | ------------------------------------ | ------------------------------------------------------------- |
| 1   | WT32-ETH02                           | Purchased from AliExpress                                     |
| 1   | Ctrl+Alt+Defib Hat PCB               | Manufactured by JLCPCB                                        |
| 1   | 3D Printed Case                      | STL files provided in the enclosure directory                 |
| 1   | 3D Printed Lid                       | STL files provided in the enclosure directory                 |
| 2   | 5V USB Wall Adapter with USB-C Cable | One powers the ESP32 from the UPS, one monitors utility power |
| 2   | USB-C to 2-Wire Power Cable          | Available from Amazon and other suppliers                     |
| 2   | JST PH 2.0mm Male Plug               | Typically supplied with matching female connectors            |

### Notes

The JST connectors are optional. The 5V and signal connections may be soldered directly to the PCB if preferred.

The Ctrl+Alt+Defib Hat circuit is intentionally simple and can be assembled on perf board if a PCB is not available.

If there is sufficient interest, assembled PCBs may be made available in the future.

---

## Ctrl+Alt+Defib Hat Components

| Reference | Description               | Value / Part                            |
| --------- | ------------------------- | --------------------------------------- |
| 5V-IN     | Power Input Connector     | JST PH 2.0mm Female                     |
| SIG-IN    | Utility Power Sense Input | JST PH 2.0mm Female                     |
| RST       | ESP32 Reset Header        | 2.54mm Pitch 1x2 Male Header            |
| R1        | Voltage Divider Resistor  | 100KΩ 1/4W                              |
| R2        | Voltage Divider Resistor  | 100KΩ 1/4W                              |
| C1        | Filter Capacitor          | 0.1µF Ceramic or Film                   |
| C2        | Power Supply Capacitor    | 10µF 25V Electrolytic, 2mm Lead Spacing |
| C3        | Filter Capacitor          | 0.1µF Ceramic or Film                   |
| U1        | ESP32 Connector           | 2.54mm Pitch 1x13 Female Header         |
| U2        | ESP32 Connector           | 2.54mm Pitch 1x13 Female Header         |

---

## Suppliers

### WT32-ETH02

* AliExpress

### PCB

* JLCPCB

### Connectors and Cables

* Amazon
* AliExpress

### Passive Components

Most components are common through-hole parts available from:

* DigiKey
* Mouser
* Newark
* Amazon
* AliExpress
