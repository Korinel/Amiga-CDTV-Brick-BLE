# IR Game Brick — User Guide

The **IR Game Brick** is a recreation of the Commodore CDTV Brick — an accessory
that apparently never made it into production. It provides two DB9 joystick ports
and Bluetooth mouse support, translating inputs into the CDTV infrared protocol.

> **Bluetooth requirement:** BLE mouse support requires a **Pico W** or **Pico 2 W**
> (the wireless variants). A standard Pico or Pico 2 without the CYW43 radio will
> operate in joystick-only mode regardless of firmware settings.

---

## Hardware Versions

The Game Brick exists in two versions:

| Feature | Version 1 (Green PCB) | Version 2 (Black PCB) |
|---|---|---|
| Dual DB9 joystick ports | ✓ | ✓ |
| Bluetooth mouse support | ✓ (Pico W / 2 W only) | ✓ (Pico W / 2 W only) |
| Serial debug header | — | ✓ |
| Battery rocker switch | — | ✓ |

**Version 2 additions:**

- **Serial debug header** — exposes the Pico UART pins so debug output can be
  viewed in the VS Code Serial Monitor, PuTTY, or similar.
- **Rocker switch (0 = off / 1 = on)** — disconnects the batteries (not USB power).
  Useful for rebooting the Pico or conserving battery without removing cells.

> **Important — firmware versions:** Version 2 moved the joystick and IR GPIO pins
> to accommodate the serial header. When flashing firmware, ensure you use the build
> compiled for your board version. CMakeLists.txt will build two versions one
> for each PCB: CDTV-BLE-Mouse-v2.uf2 and CDTV-BLE-Mouse-v1.uf2

---

## What You Will Need

- Three AA batteries (1.5 V alkaline or 1.2 V NiMH rechargeable)
- An M3 × 2.5 mm hex (Allen) screwdriver

---

## Assembly and Battery Installation

1. Using the hex screwdriver, carefully remove the two M3 faceplate screws.
2. **Version 2 only:** do not pull the faceplate too far — the rocker switch wires
   are attached to the PCB inside.
3. Remove the PCB from the enclosure to access the battery holder.
4. Insert three AA batteries, observing the polarity markings on the battery holder.
5. Slide the PCB back into the enclosure, seating it correctly in the guide rails.
6. Reattach the two faceplate screws — do not overtighten.

> The Brick can also be powered via USB (from a charger, power bank, or PC) without
> removing the batteries. Ensure your USB source is compatible with the Pico variant
> fitted. If using the board outside the enclosure, consider fitting PCB spacers
> through the four corner mounting holes to protect surfaces and avoid shorts on
> conductive surfaces.

---

## Connecting Joysticks

- Insert one or two Amiga DB9 joysticks into the ports on the faceplate.
- Both fire buttons are supported on each port.
- Auto-fire is **not** supported (as with the original CDTV controller).

---

## Bluetooth Mouse Pairing

Requires a Pico W or Pico 2 W. The Logitech M575 Ergo trackball is the
reference-tested device; other BLE HID mice should work.

### To pair a mouse

1. While holding **Fire on Joystick Port 2**, apply power to the Brick.
2. The Brick will detect the button at boot and enter Bluetooth pairing mode,
   indicated by the **internal LED flashing rapidly**.
3. Put your Bluetooth mouse into pairing mode (consult its manual).
4. The Brick scans for 60 seconds. If a compatible mouse is found, it pairs
   automatically — the LED goes solid, confirming a successful connection.
5. If no mouse is found within 60 seconds, the Brick falls back to joystick-only
   mode for that session. Power cycle and repeat to try again.

### Pairing and power cycles

Pairing must be repeated each time the Brick is powered off and on again.

### If the mouse disconnects mid-session

If the mouse goes out of range or is turned off, the Brick will automatically
rescan and reconnect once the mouse becomes available again — no reboot needed.

---

## Positioning for Best IR Performance

Point the Brick's infrared LED directly at the CDTV's IR sensor. The sensor is
located to the right of the CD drive, inside a white housing labelled **"Remote"**.

- The closer the Brick to the sensor, the more reliable the signal.
- Keep the line of sight clear of obstructions.
- The IR LED is on the inside of the enclosure — orient the Brick so the correct
  face points toward the CDTV sensor.

## Updates
Visit https://github.com/Korinel/ for more information and 
https://github.com/Korinel/Amiga-CDTV-Brick-BLE for firmware.

