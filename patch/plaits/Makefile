# Plaits on the Daisy Patch -- Mutable Instruments' macro-oscillator
# (plaits::Voice, all 24 models, vendored byte-identical under ../../vendor/eurorack) on
# libDaisy, loadable from the Patch's SD-card module picker.
#
#   make -C patch/plaits          build build/plaits_patch.bin
#
# Nothing here flashes. (libDaisy's program-dfu target exists; it is Tim's.)

TARGET = plaits_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy
EURORACK     := ../../vendor/eurorack

# shim/ first: it shadows plaits/user_data.h (no user wavetables: no F37x flash).
C_INCLUDES += -Ishim -I$(EURORACK)

# The module picker writes the chosen .bin into the bootloader's QSPI app
# slot, so every Patch module runs under the Daisy bootloader.
APP_TYPE  ?= BOOT_SRAM
USE_FATFS  = 1

# libDaisy compiles .cpp only: each vendored .cc is reached through a
# one-line vendor_pl_*.cpp wrapper (one per file, so file-local statics in
# upstream sources cannot collide; prefixed because elements has a
# resonator.cc too and patch-sim names objects by basename).
CPP_SOURCES = plaits_patch.cpp module_picker.cpp $(sort $(wildcard vendor_pl_*.cpp))

OPT ?= -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

