# Native test suites (host compiler, no hardware). Each firmware adds
# test/<fw>.mk defining a target named test-<fw>; `make` here runs them all.
#
# stmlib is compiled with -DTEST: its host switch (no ssat/vsqrt asm). Never
# pass TEST to a firmware build (vendor/VENDOR.md).
CXX      ?= c++
EURORACK := ../vendor/eurorack
CXXFLAGS := -std=gnu++14 -O2 -g -DTEST -I$(EURORACK) -I../src/shim -I. \
            -Wno-everything -fsanitize=address,undefined -fno-omit-frame-pointer
STMLIB_CC := $(EURORACK)/stmlib/dsp/units.cc $(EURORACK)/stmlib/dsp/atan.cc \
             $(EURORACK)/stmlib/utils/random.cc
BIN := bin

FW_TESTS :=
include $(wildcard *.mk)

.PHONY: all $(FW_TESTS)
all: $(FW_TESTS)
	@echo "all native suites passed: $(FW_TESTS)"

$(BIN):
	mkdir -p $(BIN)
