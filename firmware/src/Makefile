PROJECT = knob_firmware
SOURCES = knob_firmware.c
TARGET  := knob_firmware

TARGET_MCU ?= CH32V003

include ../ch32fun/ch32fun.mk

all   : flash
flash : cv_flash
clean : cv_clean
