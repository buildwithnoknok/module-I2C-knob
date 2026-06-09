PROJECT = knob_firmware
SOURCES = knob_firmware.c
TARGET  := knob_firmware

TARGET_MCU ?= CH32V003

# Link the application at the 0x1000 offset so it runs under the shared noknok
# I2C bootloader (which owns the low 4 KB of flash). See app.ld.
LINKER_SCRIPT := app.ld

include ../ch32fun/ch32fun.mk

# Default target builds the offset-linked image (flashed over I2C, not SWD).
# `make flash` still does a one-off SWD flash for bench bring-up if needed.
all   : build
flash : cv_flash
clean : cv_clean
