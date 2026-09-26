FW_TESTS += test-elements
# test_elements.cpp #includes the vendored resources.cc itself (it needs
# the original sample arrays with their sizes), so resources.cc is not
# listed here.
ELEMENTS_CC := $(filter-out %/resources.cc,$(wildcard $(EURORACK)/elements/dsp/*.cc))
ELEMENTS_ES := ../src/elements/elements_samples.cpp

$(BIN)/elements_samples_tool: ../tools/elements_samples.cpp $(ELEMENTS_ES) | $(BIN)
	$(CXX) $(CXXFLAGS) -I../src/elements $^ -o $@

$(BIN)/elements.smp: $(BIN)/elements_samples_tool
	./$< $@

$(BIN)/test_elements: test_elements.cpp $(ELEMENTS_CC) $(ELEMENTS_ES) $(STMLIB_CC) | $(BIN)
	$(CXX) $(CXXFLAGS) -I../src/elements $^ -o $@

test-elements: $(BIN)/test_elements $(BIN)/elements.smp
	./$(BIN)/test_elements $(BIN)/elements.smp
