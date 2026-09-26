# =============================================================================
# mi-alchemy -- Mutable Instruments Clouds, Elements, Marbles and Plaits for
# the Hermetic Modular Alchemy Lab V2, one repository, four firmwares.
#
#   make libdaisy            build lib/libDaisy once after cloning
#   make FW=clouds           build-clouds/clouds_alchemy.bin (BOOT_SRAM)
#   make every               all four, one after another
#   make test                native suites (no hardware), all four engines
#   make stage               copy each built .bin to ../daisy-sdk/alchemy-lab/
#                            as <fw>_alchemy-<hash>-front.bin (never the card)
#
# FW is one of: clouds elements marbles plaits. Each has src/<fw>/fw.mk with
# its sources and flags; everything else here is common.
#
# Build flags:
#   BENCH_USB=1   HostLink on the Seed's micro-USB instead of the front USB-C.
#                 NOT for racking. BUILD_DIR defaults to build-<fw>-bench.
#
# Nothing here flashes unless program-live is named. Never build with the
# module attached in DFU (memory feedback_no_builds_with_board_in_dfu).
# =============================================================================

FWS := clouds elements marbles plaits warps meld
FW  ?= clouds
ifeq ($(filter $(FW),$(FWS)),)
$(error FW must be one of: $(FWS) (got '$(FW)'))
endif

TARGET = $(FW)_alchemy

BOARD ?= v2
ifneq ($(BOARD),v2)
$(error mi-alchemy is V2-only (Marbles needs the V2 DAC routing))
endif

ALCHEMY_DIR  = lib/alchemy-sdk
LIBDAISY_DIR = lib/libDaisy
EURORACK     = vendor/eurorack

# libDaisy's core Makefile assigns BUILD_DIR unconditionally; override wins.
ifeq ($(BENCH_USB),1)
override BUILD_DIR := build-$(FW)-bench
else
override BUILD_DIR := build-$(FW)
endif

# ── Common sources ──────────────────────────────────────────────────────────
CPP_SOURCES = src/$(FW)/$(FW)_alchemy.cpp src/common/picker.cpp
CPP_SOURCES += $(sort $(shell find $(ALCHEMY_DIR)/framework/src -name '*.cpp'))
CPP_SOURCES += $(sort $(wildcard $(ALCHEMY_DIR)/hardware/alchemy-lab/v2/src/*.cpp))

# Engine sources in upstream's .cc, compiled by the rule at the bottom.
# fw.mk appends to CC_SOURCES / FW_DEFS and may set FW_CC_OPT.
# stmlib's translation units: units.cc (lut_pitch_ratio_*), atan.cc
# (atan_lut) and random.cc (Random::rng_state_); --gc-sections drops the
# ones an engine does not use.
CC_SOURCES := \
    $(EURORACK)/stmlib/dsp/units.cc \
    $(EURORACK)/stmlib/dsp/atan.cc \
    $(EURORACK)/stmlib/utils/random.cc
FW_DEFS    :=
FW_CC_OPT  := -O3
include src/$(FW)/fw.mk

C_INCLUDES += \
    -Isrc/common \
    -Isrc/$(FW) \
    -Isrc/shim \
    -I$(EURORACK) \
    -I$(ALCHEMY_DIR)/framework/include \
    -I$(ALCHEMY_DIR)/hardware/include \
    -I$(ALCHEMY_DIR)/hardware/alchemy-lab/v2/include

C_DEFS += -DALCHEMY_BOARD_V2 $(FW_DEFS)

MI_VERSION  := $(shell cat VERSION 2>/dev/null || echo 0.0.0)
MI_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null || echo dev)
C_DEFS += -DMI_VERSION=\"$(MI_VERSION)\" -DMI_GIT_HASH=\"$(MI_GIT_HASH)\"

ifeq ($(BENCH_USB),1)
C_DEFS += -DMI_BENCH_USB
endif

# ── Daisy bootloader build (BOOT_SRAM), as every Alchemy firmware ────────────
USE_FATFS    = 1
APP_TYPE     = BOOT_SRAM
LDSCRIPT     = src/common/alchemy.lds
CPP_STANDARD = -std=gnu++17
OPT          = -O3

SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

.DEFAULT_GOAL := all

# newlib-nano prints nothing for "%f" unless _printf_float is linked.
override LDFLAGS += -u _printf_float

# ── Upstream .cc files ──────────────────────────────────────────────────────
# libDaisy only knows .c/.cpp. Its link recipe expands $(OBJECTS) when it
# runs, so appending here reaches the link line; the extra prerequisite line
# makes the objects build first. FW_CC_OPT replaces -O3 for the engine only
# (Plaits and Elements trade speed for size to fit the 480 KB SRAM image).
CC_OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(CC_SOURCES:.cc=.o)))
OBJECTS   += $(CC_OBJECTS)
vpath %.cc $(sort $(dir $(CC_SOURCES)))

$(BUILD_DIR)/%.o: %.cc Makefile src/$(FW)/fw.mk | $(BUILD_DIR)
	$(CXX) -c $(filter-out $(OPT),$(CPPFLAGS)) $(FW_CC_OPT) $(CPP_STANDARD) -w $< -o $@

$(BUILD_DIR)/$(TARGET).elf: $(CC_OBJECTS)

# Two .cc files with one basename would silently share an object.
_DUP := $(sort $(foreach n,$(notdir $(CC_SOURCES:.cc=)),$(if $(word 2,$(filter $(n),$(notdir $(CC_SOURCES:.cc=)))),$(n))))
ifneq ($(_DUP),)
$(error duplicate engine basenames in $(FW): $(_DUP))
endif

# The commit hash is a -D on every compile line and make does not track
# flags: retire the one object that bakes it in whenever HEAD moves
# (smack-alchemy once shipped a bin reporting the previous commit).
HASH_STAMP := $(BUILD_DIR)/.hash-$(MI_GIT_HASH)
ifeq ($(wildcard $(HASH_STAMP)),)
_HASH_GUARD := $(shell rm -f $(BUILD_DIR)/$(TARGET).o $(BUILD_DIR)/$(TARGET).d $(BUILD_DIR)/.hash-* 2>/dev/null; mkdir -p $(BUILD_DIR); touch $(HASH_STAMP))
endif

# A linker-script change must relink (belt-alchemy's lesson).
$(BUILD_DIR)/$(TARGET).elf: $(LDSCRIPT)

.PHONY: libdaisy every test stage size
libdaisy:
	$(MAKE) -C $(LIBDAISY_DIR)

every:
	@set -e; for f in $(FWS); do $(MAKE) --no-print-directory FW=$$f; done

size:
	@for f in $(FWS); do e=build-$$f/$${f}_alchemy.elf; [ -f $$e ] && arm-none-eabi-size -A $$e | awk -v f=$$f '/^\.(text|rodata|data|itcm)|isr/{s+=$$2} END{printf "%-9s image %7d B of 491520 (%.1f%%)\n", f, s, s*100/491520}'; done; true

STAGE_DIR ?= $(HOME)/tim-os/daisy-sdk/alchemy-lab
stage:
	@for f in $(FWS); do b=build-$$f/$${f}_alchemy.bin; [ -f $$b ] && cp -v $$b $(STAGE_DIR)/$${f}_alchemy-$(MI_GIT_HASH)-front.bin; done; true
	@# Elements' exciter samples live on the card as /mi/elements.smp
	@[ -f build-elements/elements.smp ] && cp -v build-elements/elements.smp $(STAGE_DIR)/elements-$(MI_GIT_HASH).smp; true

test:
	$(MAKE) -C test

# Reboot the running module over HostLink and flash it. Tim's step, pinned:
# dfu-util -S <serial> (memory feedback_pin_dfu_util_to_device).
USBPID ?= df11
.PHONY: program-live
program-live: all
	@test -n "$(DFU_SERIAL)" || (echo "set DFU_SERIAL (dfu-util -l lists it)"; false)
	node $(ALCHEMY_DIR)/tools/hostlink-cli/hostlink.mjs reboot bootloader
	dfu-util -w -S $(DFU_SERIAL) -a 0 -s $(FLASH_ADDRESS):leave -D $(BUILD_DIR)/$(TARGET_BIN) -d ,0483:$(USBPID)
