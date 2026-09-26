FW_TESTS += test-marbles
MARBLES_CC := $(EURORACK)/marbles/resources.cc \
              $(EURORACK)/marbles/ramp/ramp_extractor.cc \
              $(wildcard $(EURORACK)/marbles/random/*.cc)
$(BIN)/test_marbles: test_marbles.cpp ../src/marbles/marbles_engine.h ../src/marbles/preset_scales.h $(MARBLES_CC) $(STMLIB_CC) | $(BIN)
	$(CXX) $(CXXFLAGS) -I../src/marbles test_marbles.cpp $(MARBLES_CC) $(STMLIB_CC) -o $@
test-marbles: $(BIN)/test_marbles
	./$(BIN)/test_marbles
