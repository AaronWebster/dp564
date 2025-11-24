# Dolby DP564 Arduino Remote Control

**Uses an Arduino ESP8266 / ESP32**

This project provides a network-based remote control for the **Dolby DP564 Multichannel Audio Decoder**. It runs on an ESP8266 or ESP32 microcontroller, automatically discovers the target device on the local network, and allows control via a Serial Command Line Interface (CLI).

## Features

* **Zero-Config WiFi:** Scans for the strongest **OPEN** WiFi network and auto-connects (no hardcoded SSID/Password).
* **Device Auto-Discovery:** Scans the local subnet for devices with **Port 4444** open.
* **MAC Address Filtering:** Validates the target is a Dolby device using specific OUI (Organizationally Unique Identifier) signatures.
* **Bi-directional Control:**
    * Set Volume, Source, and Dim (Mute).
    * Receives unsolicited updates (e.g., if the physical volume knob on the unit is turned).
* **Keep-Alive:** Implements the proprietary heartbeat protocol to maintain the TCP connection.

## Hardware Requirements

* **Microcontroller:** ESP8266 or ESP32 Development Board.
* **Connection:** USB (for Serial Monitor control and power).

## Usage

1.  Flash the sketch to the microcontroller.
2.  Open the Serial Monitor (Baud Rate: **115200**).
3.  The device will scan for WiFi, connect, and scan the subnet for the DP564.
4.  Once "Handshake complete" appears, use the following commands:

| Command | Example | Description |
| :--- | :--- | :--- |
| `volume <db>` | `volume -20.5` | Sets Master Volume (-95.0 to 0.0 dB). |
| `dim <state>` | `dim on` | Toggles the Dim (Mute) state. |
| `source <name>` | `source optical` | Switch input (aes1, aes2, optical, streaming). |
| `status` | `status` | Prints current volume, source, and dim state. |

---

## DP564 Binary Protocol (Reverse Engineered)

The communication occurs over **TCP Port 4444**. Below are the "MAYBE" packet types and definitions identified during Wireshark analysis.

### 1. Connection & Handshake
Upon establishing a TCP connection, the client must send a two-step handshake.

* **Handshake 1:** `00 00 00 05`
* **Handshake 2:** `03`
* **Heartbeat:** `00 00 00 05 04`
    * *Note:* Both client and server send this approx. every 10 seconds to keep the socket open.

### 2. Command Structure
Commands sent from Client to DP564 usually follow this pattern:
`[PRE_CMD] + [CMD_PREFIX] + [PAYLOAD_BYTE]`

* **Pre-Command:** `00 00 00 0a` (Sent immediately before specific command strings).

| Function | Command Prefix | Payload Logic |
| :--- | :--- | :--- |
| **Set Volume** | `02 03 12 00 00` | `192 + (dB * 2)` (Hex) |
| **Set Dim** | `02 05 13 00 00` | `01` (On) / `00` (Off) |
| **Set Source** | `02 03 01 00 00` | See Source Map below |

### 3. Response / Acknowledgment Structure
Responses from the DP564 usually begin with a specific header length indicator.

| Function | ACK Prefix | Last Byte (Payload) |
| :--- | :--- | :--- |
| **Volume ACK** | `00 00 00 0b 04 03 12 01 02 00` | Current Vol (See math below) |
| **Dim ACK** | `00 00 00 0b 04 05 13 01 02 00` | `01` (Dimmed) / `00` (Open) |
| **Source ACK** | `00 00 00 0b 04 03 01 01 02 00` | Source ID |
| **Knob Turn**\* | `00 00 00 0b 04 03 14 01 02 00` | New Vol (See math below) |

*\*Note: The "Knob Turn" packet is sent unsolicited by the DP564 when the physical front-panel fader is moved.*

### 4. Data Conversion

#### Volume Math
The DP564 uses a single byte integer to represent volume, where **192** equals **0.0 dB**. The scale moves in **0.5 dB** increments.

* **dB to Byte:** `Byte = 192 + (dB_Value * 2)`
* **Byte to dB:** `dB_Value = (Byte - 192) / 2.0`

*Examples:*
* 0.0 dB = `192` (`0xC0`)
* -10.0 dB = `172` (`0xAC`)
* -95.0 dB = `2` (`0x02`)

#### Source Map (Payload Bytes)
* `0x00` : **aes1**
* `0x01` : **aes2**
* `0x02` : **optical**
* `0x03` : **streaming**

### 5. Discovery Filter (MAC Addresses)
The auto-discovery logic looks for port 4444 and verifies the device against these known Dolby OUI prefixes:
* `00:12:A6` (Dolby Australia)
* `00:D0:46` (Dolby Laboratories, Inc.)
* `70:B3:D5:3F:50:xx`
* `70:B3:D5:73:A0:xx`
* `D4:25:CC:80:xx:xx`
