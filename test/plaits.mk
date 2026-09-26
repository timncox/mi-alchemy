FW_TESTS += test-plaits
PLAITS_CC := $(EURORACK)/plaits/resources.cc \
             $(shell find $(EURORACK)/plaits/dsp -name '*.cc' | sort)
$(BIN)/test_plaits: test_plaits.cpp $(PLAITS_CC) $(STMLIB_CC) | $(BIN)
	$(CXX) $(CXXFLAGS) $^ -o $@
test-plaits: $(BIN)/test_plaits
	./$(BIN)/test_plaits
