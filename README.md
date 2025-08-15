# Meshtastic Watcher

Ultra-low-power **MSP430** firmware that drives a **digital output** to simulate a button press
on a Meshtastic node.

- **Timer**: every ~12 hours it toggles the output to simulate a press.
- **Local button**: pressing it also triggers a simulated press.
- **LPM3** between events for very low sleep current.

---

## Motivation

This project is a **watchdog** for a Meshtastic Heltec V3 node in battery + solar setups. After a full
battery drain, the node may power up but **fail to rejoin** the mesh. Cause is unknown, but it seems
to enter a deep power-down indefinitely. A manual button press typically recovers it. Meshtastic
Watcher automates that press on a schedule and on demand. In my location, full battery recharge via
solar takes no more than 12 hours.

**Note on Heltec V3**  
If you reassign the Meshtastic button to a different GPIO, the built-in Heltec button may no longer
work. As a consequence, the MSP430’s local button becomes the practical way to issue user presses
and is forwarded to the reassigned Meshtastic input.

---

## Features

- Simulated press to the Meshtastic node:
  - **Automatic**: one press about every **12 hours** (best-effort with VLO).
  - **Manual**: press the MSP430’s local button.
- Digital output at 3.3 V; default MSP430 pin **P1.0** to Meshtastic button GPIO.
- Button input on **P1.3** with internal pull-up and firmware debounce.
- Timing via **Watchdog Timer** on **ACLK = VLO** (no crystal required).
- Implemented in **C** bare-metal with **PlatformIO**.

---

## Hardware

- MCU: `MSP430G2553` (adaptable to other MSP430s).
- Power: 1.8–3.6 V; prefer a low-IQ LDO if regulating.
- Connections:
  - MSP430 **P1.3** ↔ local push button to GND.
  - MSP430 **P1.0** ↔ Meshtastic Heltec V3 reassigned button GPIO; 220–1 kΩ series recommended.
  - Common **GND** between MSP430 and Heltec V3.
- If the Meshtastic input expects **open-drain**, use a small NPN/MOSFET or optocoupler.

---

## Low-Power Design

- LPM3 between events; unused pins as outputs driven low.
- No always-on LEDs; only temporary diagnostics.
- Target sleep current: ≤ 2 µA at 3 V on a clean board.

---

## Build & Upload

1. Install [PlatformIO](https://platformio.org/).
2. Connect MSP430 LaunchPad or compatible programmer.
3. Build:

```bash
   pio run
```

4. Upload:

```bash
   pio run --target upload
```

---

## Configuration

* Pins in `src/main.c`: `OUTPUT_PIN_BIT` (default P1.0) and `BUTTON_PIN_BIT` (default P1.3).
* Period in `src/main.c`: `SECONDS_PER_TOGGLE` (default `43200` ≈ 12 h with crystal; with VLO it’s
  approximate).

### Optional field calibration (no crystal)

If you want the “\~12 h” to be closer to target in your environment:

1. Let the device run for 24 h; measure the actual interval between auto presses.
2. Compute: `new_seconds = old_seconds * (target_interval / measured_interval)`.
3. Update `SECONDS_PER_TOGGLE` with `new_seconds` and rebuild.

---

## Limitations

* With VLO timing, drift is expected; temperature and unit-to-unit variation will affect cadence.
* Heltec V3 GPIO and wake sources may change in future firmware; if a proper wake source becomes
  available, this watcher may no longer be needed.

---

## License

MIT License – see [LICENSE](LICENSE) for details.
