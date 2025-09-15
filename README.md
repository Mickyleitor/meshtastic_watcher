# Meshtastic Watcher

Ultra-low-power **MSP430** firmware that emits an **active-LOW pulse** to simulate a Meshtastic button press.

* **Schedule**; one pulse every `PULSE_INTERVAL_MIN` minutes; default is **12 hours**.
* **Pulse width**; default **500 ms**.
* **Output style**; open-drain behavior on **P1.0**; idle Hi-Z; only driven LOW during the pulse.
* **Power**; **LPM3** between \~30 s timer ticks; \~0.1 µA typical in sleep excluding the brief pulse.

---

## Motivation

Some Heltec V3 nodes in solar setups fail to rejoin the mesh after a deep discharge; a manual button press usually recovers them. This firmware schedules that press using a low-power MSP430 so the node periodically gets a wake-nudge without user interaction.

---

## How it works

* **Timer_A** runs from **ACLK = VLO**; interrupts about every **30 s**.
  The ISR accumulates ticks until the requested interval elapses; then it emits a single LOW pulse.
* **Open-drain style output**; P1.0 is an input when idle; for the pulse it becomes output-LOW for `PULSE_MS`, then returns to input.
* **Low power**; CPU sleeps in **LPM3** between interrupts; DCO at 1 MHz is only enabled briefly to time the pulse with a cycle delay.
* **Board hygiene**; all unused pins are outputs driven LOW to minimize leakage.

---

## Features

* Automatic simulated press on a fixed cadence; default every **12 hours**.
* Active-LOW pulse; **500 ms** by default; adjustable at build time.
* No external crystal required; uses **VLO**; cadence is approximate without calibration.

---

## Hardware

* MCU; `MSP430G2553` by default; portable to similar MSP430s.
* Power; 1.8–3.6 V; use a low-IQ regulator if needed.
* Connections:

  * **P1.0** → Meshtastic button GPIO; the target must provide a pull-up; add a 220–1 kΩ series resistor if desired.
  * **GND** → common ground with the Meshtastic node.
* If the Meshtastic input must be **open-drain**, this firmware already idles Hi-Z; if strict open-drain is required at all times, a small NPN or MOSFET works as a buffer.

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

Edit constants at the top of `src/main.c`.

* `PULSE_INTERVAL_MIN`; minutes between pulses; default `60 * 12`.
* `PULSE_MS`; pulse width in milliseconds; default `500`.
* `PULSE_PIN_BIT`; output pin bit; default `BIT0` for **P1.0**.
* Timing base:

  * `ACLK_VLO_HZ`; nominal VLO frequency; default `11805` Hz; used to derive a \~30 s ISR tick.
  * `BASE_PERIOD_S`; tick period in seconds; default `30`.

---

## Low-power design

* LPM3 between interrupts; short wake for the ISR and the pulse.
* Unused pins configured as outputs driven LOW.
* No always-on LEDs.
* Target sleep current; \~0.1 µA typical at 3 V on a clean board; excludes the pulse window and any target pull-ups.

---

## Limitations

* VLO drifts with temperature and voltage; expect cadence variation unless calibrated.
* Heltec V3 GPIO behavior may change with firmware; if Meshtastic adds a reliable wake source, this watcher may become unnecessary.

---

## License

MIT License; see [LICENSE](LICENSE).
