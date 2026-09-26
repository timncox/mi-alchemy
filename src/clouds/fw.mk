# Clouds: GranularProcessor at its native 32 kHz, 32-frame blocks.
CC_SOURCES += \
    $(EURORACK)/clouds/resources.cc \
    $(EURORACK)/clouds/dsp/granular_processor.cc \
    $(EURORACK)/clouds/dsp/correlator.cc \
    $(EURORACK)/clouds/dsp/mu_law.cc \
    $(EURORACK)/clouds/dsp/pvoc/stft.cc \
    $(EURORACK)/clouds/dsp/pvoc/phase_vocoder.cc \
    $(EURORACK)/clouds/dsp/pvoc/frame_transformation.cc
