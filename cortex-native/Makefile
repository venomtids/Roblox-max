# Cortex Native AI — build 100% C++20, sem Python, Node.js ou geradores externos.
CXX ?= g++

CPPFLAGS := -Iinclude
COMMON_FLAGS := -std=c++20 -pthread -Wall -Wextra -Wpedantic -Wconversion -Wshadow
RELEASE_FLAGS := -O3 -DNDEBUG -march=native -flto
DEBUG_FLAGS := -O0 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer
SANITIZE_FLAGS := -O1 -g3 -fsanitize=address,undefined -fno-omit-frame-pointer

BUILD_DIR := build
SRC := src/core.cpp src/tokenizer.cpp src/neural_model.cpp src/memory.cpp src/quality.cpp \
       src/knowledge.cpp src/agent.cpp src/connectivity.cpp src/local_model.cpp \
       src/reasoner.cpp src/self_editor.cpp src/web_server.cpp src/web_ui.cpp
HEADERS := $(wildcard include/cortex/*.hpp)
CLI_SRC := $(SRC) src/main.cpp
TEST_SRC := $(SRC) tests/test_main.cpp

# Winsock é parte nativa do Windows; Linux/macOS não precisam de biblioteca adicional.
ifeq ($(OS),Windows_NT)
LDLIBS += -lws2_32 -lshell32
endif

.PHONY: all release debug test sanitize-test clean

all: release

release: $(BUILD_DIR)/cortex

debug: $(BUILD_DIR)/cortex-debug

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/cortex: $(CLI_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(RELEASE_FLAGS) $(CLI_SRC) -o $@ $(LDLIBS)

$(BUILD_DIR)/cortex-debug: $(CLI_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(DEBUG_FLAGS) $(CLI_SRC) -o $@ $(LDLIBS)

$(BUILD_DIR)/cortex-tests: $(TEST_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) -O2 $(TEST_SRC) -o $@ $(LDLIBS)

$(BUILD_DIR)/cortex-tests-sanitize: $(TEST_SRC) $(HEADERS) | $(BUILD_DIR)
	$(CXX) $(CPPFLAGS) $(COMMON_FLAGS) $(SANITIZE_FLAGS) $(TEST_SRC) -o $@ $(LDLIBS)

test: $(BUILD_DIR)/cortex-tests
	./$(BUILD_DIR)/cortex-tests

sanitize-test: $(BUILD_DIR)/cortex-tests-sanitize
	ASAN_OPTIONS=detect_leaks=1:strict_string_checks=1 UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD_DIR)/cortex-tests-sanitize

clean:
	rm -rf $(BUILD_DIR)
