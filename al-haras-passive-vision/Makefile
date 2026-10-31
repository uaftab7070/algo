# Host build:   make            (or scripts/build.sh)
# Cross build:  make BUILD=build/rv1106 CXX=<SDK toolchain>-g++ (toolchain name is verified per SDK; see docs/sdk-compatibility.md)
CXX ?= c++
BUILD ?= build/host
UNAME_S := $(shell uname -s)
TARGET_OS ?= $(UNAME_S)
VERSION := $(shell cat VERSION 2>/dev/null || echo 0.0.0-unversioned)
SOURCES_ALL := $(sort $(wildcard src/*/*.cpp))
SHA256 := $(shell command -v sha256sum >/dev/null 2>&1 && echo sha256sum || echo "shasum -a 256")
# Content hash of the exact sources compiled; embedded in binaries and reports.
BUILD_ID ?= $(shell cat $(SOURCES_ALL) $(sort $(wildcard src/*/*.hpp)) Makefile | $(SHA256) | cut -c1-16)

# CXXFLAGS/LDFLAGS may be overridden (e.g. sanitizers); the mandatory flags below cannot.
CXXFLAGS ?= -O2 -g
LDFLAGS ?=
REQUIRED_CXXFLAGS := -std=c++17 -Wall -Wextra -Wpedantic -Werror -Wshadow -Wno-unused-parameter
REQUIRED_CPPFLAGS := -Isrc -DAHPV_VERSION='"$(VERSION)"' -DAHPV_BUILD_ID='"$(BUILD_ID)"'
ALL_CXXFLAGS = $(REQUIRED_CPPFLAGS) $(CPPFLAGS) $(REQUIRED_CXXFLAGS) $(CXXFLAGS)
ALL_LDFLAGS = $(LDFLAGS) -pthread
SANITIZE_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all

ifneq ($(TARGET_OS),Linux)
SOURCES_LIB := $(filter-out src/app/% src/capture/v4l2_source.cpp,$(SOURCES_ALL))
else
SOURCES_LIB := $(filter-out src/app/%,$(SOURCES_ALL))
endif
OBJECTS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(SOURCES_LIB))
APP_OBJECTS := $(patsubst src/%.cpp,$(BUILD)/obj/%.o,$(wildcard src/app/*.cpp))
TEST_SOURCES := $(sort $(wildcard tests/unit/*.cpp))
TEST_OBJECTS := $(patsubst tests/%.cpp,$(BUILD)/obj/tests/%.o,$(TEST_SOURCES))

.DEFAULT_GOAL := all
.PHONY: all test clean sanitize print-build-id FORCE

print-build-id:
	@echo $(BUILD_ID)
all: $(BUILD)/ahpv $(BUILD)/ahpv-unit-tests

src/preview/ui_assets.cpp: src/preview/ui/index.html src/preview/ui/app.js src/preview/ui/app.css scripts/embed_ui.sh
	sh scripts/embed_ui.sh

# Recompile everything when the build ID, compiler or flags change, so the ID
# embedded in a binary always matches BUILD-INFO.txt (DAT-006).
$(BUILD)/.build-stamp: FORCE
	@mkdir -p $(BUILD)
	@echo '$(BUILD_ID) $(CXX) $(ALL_CXXFLAGS) $(ALL_LDFLAGS)' | cmp -s - $@ || echo '$(BUILD_ID) $(CXX) $(ALL_CXXFLAGS) $(ALL_LDFLAGS)' > $@

FORCE:

$(BUILD)/obj/%.o: src/%.cpp $(BUILD)/.build-stamp
	@mkdir -p $(dir $@)
	$(CXX) $(ALL_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/obj/tests/%.o: tests/%.cpp $(BUILD)/.build-stamp
	@mkdir -p $(dir $@)
	$(CXX) $(ALL_CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/libahpv.a: $(OBJECTS)
	@rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/ahpv: $(APP_OBJECTS) $(BUILD)/libahpv.a
	$(CXX) $(REQUIRED_CXXFLAGS) $(CXXFLAGS) $^ $(ALL_LDFLAGS) -o $@

$(BUILD)/ahpv-unit-tests: $(TEST_OBJECTS) $(BUILD)/libahpv.a
	$(CXX) $(REQUIRED_CXXFLAGS) $(CXXFLAGS) $^ $(ALL_LDFLAGS) -o $@

test: all
	$(BUILD)/ahpv-unit-tests

# AddressSanitizer + UndefinedBehaviorSanitizer build in build/asan.
sanitize:
	$(MAKE) BUILD=build/asan CXXFLAGS="$(SANITIZE_FLAGS)"

clean:
	rm -rf $(BUILD)

-include $(OBJECTS:.o=.d) $(APP_OBJECTS:.o=.d) $(TEST_OBJECTS:.o=.d)
