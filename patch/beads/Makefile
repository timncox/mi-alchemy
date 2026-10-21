# Beads on the Daisy Patch, for the SD-card module picker.
#
#   make -C patch/beads         build/beads_patch.bin (BOOT_SRAM)
#
# Never flashes anything: copy build/beads_patch.bin to the card as
# /modules/beads.bin and load it from any module's "Mods" row.
# Requires libDaisy built at LIBDAISY_DIR. The DSP is vendor/eurorack's,
# reached through one-line vendor_*.cpp wrappers (libDaisy's Makefile only
# knows .cpp; one wrapper per .cc so file-local statics cannot collide).

TARGET = beads_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# No -DTEST: on the module stmlib's Clip16 etc. should be the Cortex-M
# instructions, and TEST collides with a name in the ST headers.
C_INCLUDES += -I../common/shim -I../common -I../../src/beads -I../../vendor/eurorack
# resample_3_2.h and beads_laws.h come from the Lab build (src/beads); they
# are plain C++ with no Alchemy SDK dependency.

APP_TYPE ?= BOOT_SRAM
USE_FATFS = 1

CPP_SOURCES = beads_patch.cpp module_picker.cpp $(sort $(wildcard vendor_*.cpp))

OPT = -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
