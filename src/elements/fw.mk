# Elements: elements::Part at its native 32 kHz, 16-frame blocks.
#
# The image does NOT carry the two exciter sample tables (338 KB of the
# vendored resources.cc): they load from the SD card into SDRAM at boot
# (elements_samples.h). So the image compiles a copy of resources.cc with
# exactly those two array definitions cut out, generated below from the
# byte-identical vendored file; elements_storage.cpp supplies the symbols.
# If the cut ever misses, the link fails loudly (duplicate definitions),
# and if it takes too much, undefined references do.

CPP_SOURCES += \
    src/elements/elements_samples.cpp \
    src/elements/elements_storage.cpp

ELEMENTS_RES_IMG := $(BUILD_DIR)/elements_resources_img.cc

CC_SOURCES += \
    $(ELEMENTS_RES_IMG) \
    $(EURORACK)/elements/dsp/exciter.cc \
    $(EURORACK)/elements/dsp/multistage_envelope.cc \
    $(EURORACK)/elements/dsp/ominous_voice.cc \
    $(EURORACK)/elements/dsp/part.cc \
    $(EURORACK)/elements/dsp/resonator.cc \
    $(EURORACK)/elements/dsp/string.cc \
    $(EURORACK)/elements/dsp/tube.cc \
    $(EURORACK)/elements/dsp/voice.cc

# Cut from the line opening each array to the first "};" after it.
$(ELEMENTS_RES_IMG): $(EURORACK)/elements/resources.cc src/elements/fw.mk
	@mkdir -p $(dir $@)
	awk '/^const int16_t smp_(sample_data|noise_sample)\[\] = \{/ { skip = 1; print "// mi-alchemy: " $$3 " cut (SD card -> SDRAM, elements_storage.cpp)"; next } \
	     skip && /^\};/ { skip = 0; next } !skip { print }' $< > $@
	@test $$(grep -c 'mi-alchemy: smp_' $@) -eq 2 || (echo "resources cut did not find both arrays"; rm -f $@; false)

# Explicit rule: the generated file lives in the build dir, which the
# pattern rule's vpath lookup cannot see before it exists.
$(BUILD_DIR)/elements_resources_img.o: $(ELEMENTS_RES_IMG)
	$(CXX) -c $(filter-out $(OPT),$(CPPFLAGS)) $(FW_CC_OPT) $(CPP_STANDARD) -w $< -o $@

# The card file, written by a host build of tools/elements_samples.cpp.
HOSTCXX ?= c++
ELEMENTS_SMP := $(BUILD_DIR)/elements.smp

$(BUILD_DIR)/elements_samples_tool: tools/elements_samples.cpp src/elements/elements_samples.cpp src/elements/elements_samples.h $(EURORACK)/elements/resources.cc
	@mkdir -p $(dir $@)
	$(HOSTCXX) -std=gnu++14 -O1 -DTEST -I$(EURORACK) -Isrc/elements tools/elements_samples.cpp src/elements/elements_samples.cpp -o $@

$(ELEMENTS_SMP): $(BUILD_DIR)/elements_samples_tool
	$< $@

all: $(ELEMENTS_SMP)
