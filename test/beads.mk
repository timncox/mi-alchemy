FW_TESTS += test-beads
BEADS_CC := $(EURORACK)/beads/resources.cc $(EURORACK)/beads/gain_control.cc \
            $(wildcard $(EURORACK)/beads/dsp/*.cc)
# src/shim first: its beads/drivers/codec.h must shadow upstream's, which
# needs the STM32H7 HAL (the firmware's include order does the same).
$(BIN)/test_beads: test_beads.cpp $(BEADS_CC) $(STMLIB_CC) ../src/beads/resample_3_2.h ../src/beads/beads_laws.h | $(BIN)
	$(CXX) -I../src/shim $(CXXFLAGS) $(filter %.cpp %.cc,$^) -o $@
test-beads: $(BIN)/test_beads
	./$(BIN)/test_beads
