FW_TESTS += test-warps
WARPS_CC := $(wildcard ../vendor/parasites/warps/dsp/*.cc) ../vendor/parasites/warps/resources.cc
$(BIN)/test_warps: test_warps.cpp $(WARPS_CC) $(STMLIB_CC) ../src/warps/warps_params.h | $(BIN)
	$(CXX) $(CXXFLAGS) -I../vendor/parasites -I../src/warps $(filter-out %.h,$^) -o $@
test-warps: $(BIN)/test_warps
	./$(BIN)/test_warps
