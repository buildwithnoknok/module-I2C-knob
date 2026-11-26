# Changelog - noknok Knob module firmware

Newest first. One entry per version bump, written in the same commit as the bump. Format:
`## <version> - <date>` then `- DEV-xx - what changed and why. Breaking: yes/no.`
Convention: Confluence "Development Conventions" (SD space, page 120160257).

## 2.4.0 - 2026-11-20

- DEV-21 - answers the new standard command `0xB4` GET_ID (ADR-005): write `[0xB4]`, the next read returns the 10-byte identity `[UID x8][TYPE][CRC8]`, identical to the 0x7F enumeration reply; latched in the I2C ISR at STOP like `0xB1` (never via the main loop). `0xB1` and `PROTOCOL_VERSION` (0x01) unchanged. Adds the 20-byte application header `{"NKA1", "V003", layout 2, version, type 0x02}` at app base + 0x100 (app.ld places it; the build asserts size and position) that the DEV-22 stage-1 copies into the metadata page at VERIFY. Not tested on hardware yet (bench list in the DEV-21 comment). Breaking: no.
- DEV-21 - release binary built on the bench toolchain (gcc 14.2.0, ch32fun 95030fd), so it can be rebuilt byte for byte from this source: app 2764 B, crc32 `94bb1c7d`; full image crc32 `ddbf4529`. Same source and version as the first 2026-11-20 build, which was made on another toolchain and is not reproducible.

## Full image - 2026-11-19

- DEV-197 - new `firmware/bin/knob_full.bin`: the full flash image (stage-0 + stage-1 1.2.0 + knob firmware 2.3.0 + valid-app marker) for a first flash or recovery over SWD, recorded in `firmware/full.json`. Built from released parts by the new `tools/mk_full_image.py`; `--check` fails when the full image no longer matches `firmware/index.json`, which still points at the app-only OTA image. README: how to flash it, and why the app image must never go to `0x08000000`. No firmware change. Breaking: no.

## Baseline - 2026-10-02

- Changelog starts here. Current: knob firmware 2.3.0 (released 2026-09-11). Earlier history: git log and the README.
