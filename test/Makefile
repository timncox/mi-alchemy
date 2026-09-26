# Native test suites (host compiler, no hardware). Each firmware adds
# test/<fw>.mk defining a target named test-<fw>; `make` here runs them all.
#
# stmlib is compiled with -DTEST: its host switch (no ssat/vsqrt asm). Never
# pass TEST to a firmware build (vendor/VENDOR.md).
CXX      ?= c++
EURORACK := ../vendor/eurorack
#
# ASan checks heap and stack, NOT global arrays (-asan-globals=0): Mutable's
# code reads interpolated lookup tables with a phase that can reach exactly
# 1.0, which reads the float one past the table's end times a zero fraction
# (clouds lut_xfade_in at dry_wet 1.0, lut_window at a grain's end). It is
# harmless on the part and all over upstream, and it is vendored code we do
# not patch; with global redzones on, ASan stops at the first one.
CXXFLAGS := -std=gnu++14 -O2 -g -DTEST -I$(EURORACK) -I../src/shim -I. \
            -Wno-everything -fsanitize=address,undefined -fno-omit-frame-pointer \
            -mllvm -asan-globals=0
STMLIB_CC := $(EURORACK)/stmlib/dsp/units.cc $(EURORACK)/stmlib/dsp/atan.cc \
             $(EURORACK)/stmlib/utils/random.cc
BIN := bin

.DEFAULT_GOAL := all
FW_TESTS :=
include $(wildcard *.mk)

.PHONY: all $(FW_TESTS)
all: $(FW_TESTS)
	@echo "all native suites passed: $(FW_TESTS)"

$(BIN):
	mkdir -p $(BIN)
