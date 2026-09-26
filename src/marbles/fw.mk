# Marbles: TGenerator + XYGenerator at the native 32 kHz, 16-frame blocks.
# settings.cc / ui.cc / cv_reader.cc / drivers are not compiled: their jobs
# are done by src/marbles/marbles_engine.h and marbles_alchemy.cpp.
CC_SOURCES += \
    $(EURORACK)/marbles/resources.cc \
    $(EURORACK)/marbles/ramp/ramp_extractor.cc \
    $(EURORACK)/marbles/random/t_generator.cc \
    $(EURORACK)/marbles/random/x_y_generator.cc \
    $(EURORACK)/marbles/random/output_channel.cc \
    $(EURORACK)/marbles/random/lag_processor.cc \
    $(EURORACK)/marbles/random/quantizer.cc \
    $(EURORACK)/marbles/random/discrete_distribution_quantizer.cc
