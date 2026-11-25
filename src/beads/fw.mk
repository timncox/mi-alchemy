# Beads: Processor + Synth on upstream's 48 kHz path (ENABLE_HARDWARE_24K off,
# vendor/VENDOR.md), 12-frame blocks (Beads' 4 kHz block rate).
CC_SOURCES += \
    $(EURORACK)/beads/resources.cc \
    $(EURORACK)/beads/gain_control.cc \
    $(EURORACK)/beads/dsp/processor.cc \
    $(EURORACK)/beads/dsp/synth.cc \
    $(EURORACK)/beads/dsp/trigger_generator.cc \
    $(EURORACK)/beads/dsp/delay_controller.cc \
    $(EURORACK)/beads/dsp/delay_clock.cc \
    $(EURORACK)/beads/dsp/mu_law.cc
FW_CC_OPT := -Os

# The folder picker and Phone mode push Beads past the 480 KB SRAM image at
# -O3: the SDK's USB description, HostLink and settings code (none of it on
# the audio path) builds for size instead.
FW_OS_OBJECTS := descriptor.o describe.o host_link.o fs_extension.o presets.o settings.o
