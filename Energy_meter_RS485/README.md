# ESP32 GoodWe GM1000 Smart Meter Interface

A native ESP-IDF implementation configuring an ESP32 as a Modbus Master to extract real-time energy consumption and generation data from a standalone GoodWe GM1000 single-phase smart power meter.

## Hardware Configuration

### Components
* **MCU:** ESP32 Development Board
* **Transceiver:** MAX3485 TTL-to-RS485 module (3.3V compatible)
* **Meter:** GoodWe GM1000 Smart Power Meter (configured as Modbus Slave)

### Pin Mapping

| ESP32 Pin | MAX3485 Pin | Description |
| :--- | :--- | :--- |
| **GPIO 17** | DI (Data In) | UART Transmit (TXD) |
| **GPIO 16** | RO (Receiver Out) | UART Receive (RXD) |
| **GPIO 4** | DE / RE | RTS / Direction Control (Half-Duplex) |
| *N/A* | A | Connects to GM1000 RS485 Terminal **A** |
| *N/A* | B | Connects to GM1000 RS485 Terminal **B** |

> **Note on Grounding:** The GM1000 does not expose a signal ground screw terminal for RS485. Ensure your ESP32 power supply shares a safe mains isolation reference, or utilize an optically isolated RS485 transceiver.

---

## Modbus Communication Parameters

The interface utilizes standard Modbus RTU serial frame parameters to sync with the factory default configuration of the smart meter:

* **Baud Rate:** `9600`
* **Serial Profile:** `8N1` (8 Data Bits, No Parity, 1 Stop Bit)
* **Modbus Role:** Master
* **Target Slave ID:** `3` (GoodWe default)
* **Polling Interval:** 10 Seconds

### Target Register Mapping
The internal single-phase current transformer (CT Channel) on the GM1000 routes to the phase L3 register block:

* **Register Address:** `106` (Decimal) / `0x006A` (Hexadecimal)
* **Modbus Command:** Function Code `0x03` (Read Holding Registers)
* **Data Type:** 16-bit Signed Integer (`int16_t`)
* **Unit:** Watts ($W$)

---

## Features & Logic Tracking

* **Half-Duplex Flow Control:** Managed natively via ESP-IDF's automatic hardware RTS driver switching (`UART_MODE_RS485_HALF_DUPLEX`).
* **Direction Detection:** Automatically checks sign bits to separate grid consumption (**Import**) from solar generation feedback (**Export**).
