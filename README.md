# noknok Knob

An I²C‑controlled rotary encoder module for the noknok ecosystem.  
Designed for smooth, reliable user input such as volume control, menu navigation, and parameter adjustment.

![USB LED Module](hardware/module-I2C-rotaryencoder-front.png)
![USB LED Module](hardware/module-I2C-rotaryencoder-back.png)

---

## Overview

The **Rotary Encoder Module** uses a CH32V003 microcontroller to read a mechanical rotary encoder with an integrated push button.  
It exposes rotation steps and button events over I²C using the standard noknok connector.

Typical use cases:
- Volume control
- Menu navigation
- Parameter tuning (brightness, speed, mode selection)
- General UI input for kits and enclosures

---

## Features

- I²C communication (default address: `TBD`)
- CH32V003J4M6 microcontroller
- High‑resolution step detection
- Integrated push button support
- Debouncing handled in firmware
- 3.3V operation via noknok I²C connector
- Compact 20×20 mm PCB
- Mounting holes for enclosure integration

---

## Status

- Hardware: **v1.0**  
- Firmware: **stable**  
- Documentation: **in progress**

---

## License

TBD - to be added when the repository becomes public.
