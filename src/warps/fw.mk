# Warps (Parasites): warps::Modulator at its native 96 kHz, 60-frame blocks.
# The DSP is vendor/parasites/warps (mqtthiqs/parasites @ 32fa66f via meld),
# compiled against vendor/eurorack/stmlib: it needs nothing that stmlib
# version lacks (test/test_warps.cpp is the proof). ui.cc, cv_scaler.cc,
# settings.cc and drivers are not vendored; warps_params.h does their job.
C_INCLUDES += -Ivendor/parasites
CC_SOURCES += \
    vendor/parasites/warps/resources.cc \
    vendor/parasites/warps/dsp/filter_bank.cc \
    vendor/parasites/warps/dsp/modulator.cc \
    vendor/parasites/warps/dsp/oscillator.cc \
    vendor/parasites/warps/dsp/vocoder.cc
