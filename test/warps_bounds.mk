FW_TESTS += test-warps-bounds
# Global-array redzones ON, and a NaN/inf reaching an int conversion fatal
# (see test_warps_bounds.cpp). Own source list: make 3.81 doesn't sort
# $(wildcard *.mk), so warps.mk's WARPS_CC may not be set yet.
WARPS_BOUNDS_CC := $(wildcard ../vendor/parasites/warps/dsp/*.cc) ../vendor/parasites/warps/resources.cc
$(BIN)/test_warps_bounds: test_warps_bounds.cpp $(WARPS_BOUNDS_CC) $(STMLIB_CC) | $(BIN)
	$(CXX) $(filter-out -mllvm -asan-globals=0,$(CXXFLAGS)) -fno-sanitize-recover=float-cast-overflow \
	    -I../vendor/parasites $(filter-out %.h,$^) -o $@
test-warps-bounds: $(BIN)/test_warps_bounds
	./$(BIN)/test_warps_bounds
