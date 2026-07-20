# Hardware — noknok Knob

Hardware design files for the noknok Knob module (CH32V003J4M6 — I2C rotary encoder with integrated push button).

- KiCad project: `kicad/I2C_knob.*`
- Schematic (PDF): `module-I2C-knob-V2_20260720.pdf`
- Board renders: `module-I2C-rotaryencoder-front.png`, `module-I2C-rotaryencoder-back.png` *(pre-V2 renders — pending re-export)*

Hardware is licensed CC BY-SA 4.0 (see `../LICENSE-hardware`). Connector, flashing and mounting standards follow the [noknok Ecosystem guidelines](https://github.com/buildwithnoknok/Ecosystem) (electrical + mechanical).

---

## Hardware Change Record

### v2.0 — changes from v1.0
- **Status indicator LED on PD1** (in parallel with the SWIO flashing line — zero extra GPIO). Active-low: `+3V3_PROT → R5 (2.2 kΩ) → D2 anode; D2 cathode → PD1/SWIO`. Driven by the bootloader (off = app running, slow pulse = updating/recovering, solid = error). Parts: red 0603 LED (LCSC C2286) + 2.2 kΩ 0603 (LCSC C4190).
- **Removed both castellated edge pads** — the 5-pin flashing edge and the redundant I2C edge (unreliable to contact). I2C is via the JST-SH (Qwiic) connectors J1/J2 only.
- **New flashing interface (J3)** — noknok flash pads (`noknok_FlashPads_I2C-module_1x3_M2.5`): 3 single-side SMD pogo pads (GND / SWIO / VCC) plus an embedded M2.5 keyed mounting hole, from the noknok KiCad library in the Ecosystem repo. Pad row is offset toward the keyed hole (not centered) so a 180°-flipped clamp contact can't seat and reverse-power the board.
- **Second mounting hole (H1)** — plain `noknok_MountingHole_2.5mm_M2.5`, diagonally opposite the flash-pad hole, center (2.25, 2.25) mm from the board corner.
- **Removed the on-board I²C pull-up resistors (R2/R3)** — pull-ups now live on the host (Conductor), not per module (avoids stacking on the shared bus). The host must provide the bus pull-ups (proposed 3.3 kΩ); a noknok pull-up PCB covers third-party hosts that lack them. See the [I²C Pull-up Resistor Strategy ADR](https://noknokdev.atlassian.net/wiki/spaces/SD/pages/82280449).
- **R1 (VDD series) stays 10 Ω** — the knob has no spiky load on VDD (unlike the buzzer's beep-current 10Ω→1Ω fix), so the standard default is unchanged.
