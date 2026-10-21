# Elements on the Daisy Patch -- Mutable Instruments' modal voice
# (elements::Part, vendored byte-identical under ../../vendor/eurorack) on
# libDaisy, loadable from the Patch's SD-card module picker.
#
#   make -C patch/elements          build build/elements_patch.bin
#
# Nothing here flashes. (libDaisy's program-dfu target exists; it is Tim's.)

TARGET = elements_patch

LIBDAISY_DIR ?= $(HOME)/tim-os/daisy-sdk/libDaisy
EURORACK     := ../../vendor/eurorack

# shim/ first: it shadows elements/drivers/debug_pin.h (an F4 GPIO toggle).
C_INCLUDES += -Ishim -I$(EURORACK)

# The module picker writes the chosen .bin into the bootloader's QSPI app
# slot, so every Patch module runs under the Daisy bootloader.
APP_TYPE  ?= BOOT_SRAM
USE_FATFS  = 1

# libDaisy compiles .cpp only: each vendored .cc is reached through a
# one-line vendor_el_*.cpp wrapper (one per file, so file-local statics in
# upstream sources cannot collide; prefixed because plaits has a
# resonator.cc too and patch-sim names objects by basename).
CPP_SOURCES = elements_patch.cpp module_picker.cpp $(sort $(wildcard vendor_el_*.cpp))

OPT ?= -O2

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# The image is 466 KB of tables + code before any UI, and at -O2 throughout
# it is 490 KB of the 480 KB SRAM region's 491,520 bytes (99.7 %). -Os on
# the engine only: Elements ran at -O2 on a 168 MHz Cortex-M4 at 32 kHz; the
# H750 is a 480 MHz M7, and the header's cAA/PP meter measures what that
# costs on the first hardware boot.
$(BUILD_DIR)/vendor_el_%.o: OPT = -Os
