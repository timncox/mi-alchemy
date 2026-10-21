# Marbles on the Electrosmith Daisy Patch -- module-picker build.
#
#   make -C patch/marbles          build/marbles_patch.bin (BOOT_SRAM)
#
# Never flashes anything by itself: put the .bin on the Patch card as
# /modules/marbles.bin and pick it from any module's "mods" row.
#
# Same engine as the Alchemy Lab target (src/marbles/marbles_engine.h over
# the vendored generators at 32 kHz / 16-frame blocks); this directory only
# holds the Patch panel shim. libDaisy is the shared Patch checkout, NOT
# mi-alchemy's lib/libDaisy (that one is the Alchemy SDK's pin).

TARGET = marbles_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy
EURORACK     := ../../vendor/eurorack

APP_TYPE  ?= BOOT_SRAM
USE_FATFS  = 1

# T gates leave one block (16 samples, 0.5 ms) late, so they land after the
# DAC write of the X they belong to (the DAC is written at the end of each
# callback). The Lab needed 64 for its I2C DAC.
MARBLES_DEFS = -DMARBLES_PORT_GATE_DELAY=16
MI_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
MARBLES_DEFS += -DMI_GIT_HASH=\"$(MI_GIT_HASH)\"
C_DEFS   += $(MARBLES_DEFS)
CPP_DEFS += $(MARBLES_DEFS)
C_INCLUDES += -I$(EURORACK) -I../../src/marbles

# No -DTEST (stmlib's host switch) on the module: see vendor/VENDOR.md.
CPP_SOURCES = marbles_patch.cpp module_picker.cpp \
              vendor_resources.cpp vendor_ramp_extractor.cpp vendor_t_generator.cpp \
              vendor_x_y_generator.cpp vendor_output_channel.cpp vendor_lag_processor.cpp \
              vendor_quantizer.cpp vendor_discrete_distribution_quantizer.cpp \
              vendor_stmlib_units.cpp vendor_stmlib_atan.cpp vendor_stmlib_random.cpp

OPT = -O2
CPP_STANDARD = -std=gnu++17

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Marbles' upstream sources are not -Wall clean.
CPPFLAGS += -Wno-unused-parameter -Wno-sign-compare -Wno-unused-variable
