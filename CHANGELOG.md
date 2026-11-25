# Changelog - noknok Knob module firmware

Newest first. One entry per version bump, written in the same commit as the bump. Format:
`## <version> - <date>` then `- DEV-xx - what changed and why. Breaking: yes/no.`
Convention: Confluence "Development Conventions" (SD space, page 120160257).

## Housing notes - 2026-11-25

- DEV-280 - `mechanical/housing.json` notes said the board has M2 / 2.2 mm mounting holes; the KiCad board has M2.5 holes (drill 2.5), as `mounting[]` already said. Notes corrected, no number changed, no firmware change. Breaking: no.

## Full image - 2026-11-19

- DEV-197 - new `firmware/bin/knob_full.bin`: the full flash image (stage-0 + stage-1 1.2.0 + knob firmware 2.3.0 + valid-app marker) for a first flash or recovery over SWD, recorded in `firmware/full.json`. Built from released parts by the new `tools/mk_full_image.py`; `--check` fails when the full image no longer matches `firmware/index.json`, which still points at the app-only OTA image. README: how to flash it, and why the app image must never go to `0x08000000`. No firmware change. Breaking: no.

## Baseline - 2026-10-02

- Changelog starts here. Current: knob firmware 2.3.0 (released 2026-09-11). Earlier history: git log and the README.
