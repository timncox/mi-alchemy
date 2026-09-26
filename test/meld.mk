FW_TESTS += test-meld
MELD_CC := $(wildcard ../vendor/parasites/warps/dsp/*.cc) ../vendor/parasites/warps/resources.cc \
           $(wildcard ../vendor/warps_stock/dsp/*.cc) ../vendor/warps_stock/resources.cc
$(BIN)/test_meld: test_meld.cpp $(MELD_CC) $(STMLIB_CC) ../src/meld/meld_params.h ../src/warps/warps_params.h | $(BIN)
	$(CXX) $(CXXFLAGS) -I../vendor/parasites -I../vendor -I../src/warps -I../src/meld $(filter-out %.h,$^) -o $@
test-meld: $(BIN)/test_meld
	./$(BIN)/test_meld
