FW_TESTS += test-state-file
$(BIN)/test_state_file: test_state_file.cpp ../src/common/state_file.h | $(BIN)
	$(CXX) $(CXXFLAGS) test_state_file.cpp -o $@
test-state-file: $(BIN)/test_state_file
	./$(BIN)/test_state_file
