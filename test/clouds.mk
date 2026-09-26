FW_TESTS += test-clouds
CLOUDS_CC := $(EURORACK)/clouds/resources.cc \
             $(wildcard $(EURORACK)/clouds/dsp/*.cc) \
             $(wildcard $(EURORACK)/clouds/dsp/pvoc/*.cc)
$(BIN)/test_clouds: test_clouds.cpp $(CLOUDS_CC) $(STMLIB_CC) | $(BIN)
	$(CXX) $(CXXFLAGS) $^ -o $@
test-clouds: $(BIN)/test_clouds
	./$(BIN)/test_clouds
