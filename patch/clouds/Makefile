# Clouds on the Daisy Patch, for the SD-card module picker.
#
#   make -C patch/clouds         build/clouds_patch.bin (BOOT_SRAM)
#
# Never flashes anything: copy build/clouds_patch.bin to the card as
# /modules/clouds.bin and load it from any module's "Mods" row.
# Requires libDaisy built at LIBDAISY_DIR. The DSP is vendor/eurorack's,
# reached through one-line vendor_*.cpp wrappers (libDaisy's Makefile only
# knows .cpp; one wrapper per .cc so file-local statics cannot collide).

TARGET = clouds_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy

# No -DTEST: on the module stmlib's Clip16 etc. should be the Cortex-M
# instructions, and TEST collides with a name in the ST headers.
C_INCLUDES += -I../common/shim -I../common -I../../vendor/eurorack

APP_TYPE ?= BOOT_SRAM
USE_FATFS = 1

CPP_SOURCES = clouds_patch.cpp module_picker.cpp $(sort $(wildcard vendor_*.cpp))

OPT = -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile
