# RPI_2W_Firmware

Firmware for the Raspberry Pi Pico 2 W nodes used in Boson Plant Monitor product tracking.

## Arduino reference (serial emulator)

[`Node_Firmware_1b1.cpp`](Node_Firmware_1b1.cpp) — Earle Philhower Arduino-Pico build. Barcodes via **USB CDC Serial** from `Barcode_generator_GS1_T-0.py`.

[`Node_Debug_Diagnostics_Run.cpp`](Node_Debug_Diagnostics_Run.cpp) — Standalone diagnostics sketch.

## Pico SDK build (USB HID scanner on micro USB)

[`node_firmware/`](node_firmware/) — Production-oriented firmware:

- **1:1** behavior with `Node_Firmware_1b1.cpp` (Wi-Fi, engine TCP, JSON, ACK, queue, diagnostics).
- **USB host** on the **micro USB** port (OTG + HID keyboard-mode scanner), same approach as [`barcodescannertest`](../barcodescannertest/barcodescanner/barcodescanner.cpp).
- Compact scanner payloads are normalized to parenthesized GS1 before validation (same rules as the original `(91)` check).
- Logs on **UART** (`stdio_usb` off while USB is host). Flash UF2 via **BOOTSEL**.

### Configure

Edit Wi-Fi and engine settings at the top of [`node_firmware/node_firmware.cpp`](node_firmware/node_firmware.cpp) (`WIFI_SSID`, `WIFI_PASSWORD`, `ENGINE_HOST`, `DEVICE_ID`).

### Build

Requires Pico SDK (e.g. 2.3.x) and `PICO_SDK_PATH`:

```bash
cd node_firmware
mkdir build && cd build
cmake ..
cmake --build .
```

Output: `node_firmware.uf2`

**Production (default)** — scanner on micro USB OTG:

```bash
cmake .. -DBOSON_USB_HID_HOST=ON
```

**VS Code USB serial debug** — micro USB to PC, no HID host (bench only):

```bash
cmake .. -DBOSON_USB_HID_HOST=OFF
```

After changing this option, delete the `build` folder or reconfigure so CMake picks up stdio settings.

### Wiring

- Power/program: hold **BOOTSEL**, plug micro USB, flash UF2.
- Runtime (default build): micro USB **OTG host** → barcode scanner (HID).
- **Logs (default build):** **UART0** on **GP0 (TX)** and **GND** @ **115200** to a USB–serial adapter (micro USB is host, not a PC COM port).
- **Logs (`BOSON_USB_HID_HOST=OFF`):** Raspberry Pi Pico VS Code serial monitor on the Pico’s **USB COM port** @ **115200** (scanner host disabled).

### On-board LED (CYW43 WL)

| Indication | Meaning |
|------------|---------|
| **Breathe** (soft glow) | Wi-Fi OK, scanner mounted, idle (ready to scan) |
| **1–5 pulse groups** | Priority status (same idea as barcodescannertest): no Wi-Fi → 1; Wi-Fi → 2; engine/TCP or pending queue → 3; scanner only → 4; fatal → 5 |
| **2 quick flashes** | Barcode queued |
| **Solid ~350 ms** | Engine ACK OK |
| **Fast strobe** | Send/ACK failure (re-queued) |
