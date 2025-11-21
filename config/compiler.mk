# Shared Compiler Configuration
# Include this file in all Makefiles to ensure consistent compiler settings

ifeq ($(MAKELEVEL),0)

ifndef OS
OS = $(shell uname -s)
endif

COMMON_CXXFLAGS = -std=c++23 -stdlib=libc++ -pthread -fPIC
COMMON_CXXFLAGS += -fexperimental-library -Wall -Wextra -g
COMMON_LDFLAGS =
AR = ar
ARFLAGS = rv

# Platform-specific compiler configuration
# Use $(shell uname -s) directly to ensure OS is evaluated at parse time
ifeq ($(shell uname -s),Linux)
# Debian/Ubuntu package installation paths
CC = clang-20
CXX = clang++-20
CLANG_SCAN_DEPS = clang-scan-deps-20
CXXFLAGS = $(COMMON_CXXFLAGS) -I/usr/lib/llvm-20/include/c++/v1 -O3
ARCH = $(shell uname -m)
LDFLAGS = $(COMMON_LDFLAGS) -L/usr/lib/$(ARCH)-linux-gnu -lc++ -O3

# STD_LLVM_PREFIX is used for building std.pcm from libc++ source
# Linux: Use /usr/lib/llvm-20 (standard clang-20 installation)
STD_LLVM_PREFIX := /usr/lib/llvm-20

# Fail fast if clang-scan-deps is not available (required for module dependency generation)
ifeq ($(shell command -v $(CLANG_SCAN_DEPS) >/dev/null 2>&1 || echo 1),1)
$(error clang-scan-deps not found. Please install clang-tools-20 package)
endif
endif

ifeq ($(shell uname -s),Darwin)
# Assume clang-20+ is installed at /usr/local/llvm with libc++
# LLVM_PREFIX can be overridden via environment variable
ifndef LLVM_PREFIX
LLVM_PREFIX := /usr/local/llvm
endif

# STD_LLVM_PREFIX is used for building std.pcm from libc++ source
# macOS: Use LLVM_PREFIX (set above)
STD_LLVM_PREFIX := $(LLVM_PREFIX)

# Fail fast if LLVM is not found at expected location
ifeq ($(wildcard $(LLVM_PREFIX)/bin/clang++),)
$(error LLVM not found at $(LLVM_PREFIX). Please install clang-20+ to /usr/local/llvm)
endif

# Force LLVM to avoid picking up system 'cc'/'c++' from the environment
override CC := $(LLVM_PREFIX)/bin/clang
override CXX := $(LLVM_PREFIX)/bin/clang++
override CLANG_SCAN_DEPS := $(LLVM_PREFIX)/bin/clang-scan-deps

# Fail fast if clang-scan-deps is not available (required for module dependency generation)
ifeq ($(wildcard $(CLANG_SCAN_DEPS)),)
$(error clang-scan-deps not found at $(CLANG_SCAN_DEPS). Please install clang-20+ with clang-scan-deps)
endif

# Resolve SDK path once to avoid mixing CommandLineTools and Xcode paths
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
export SDKROOT

# Assume LLVM has libc++ installed - use LLVM's libc++ headers and libraries
# This enables using "import std;" with -fexperimental-library
override CXXFLAGS := $(COMMON_CXXFLAGS) -nostdinc++ -isystem $(LLVM_PREFIX)/include/c++/v1 -fno-implicit-modules -fno-implicit-module-maps -O3

# Add SDK and library paths
# Use LLVM's libc++ library directory with rpath for runtime linking
ifneq ($(SDKROOT),)
CXXFLAGS += -isysroot $(SDKROOT)
LDFLAGS ?= $(COMMON_LDFLAGS) -isysroot $(SDKROOT) -L$(LLVM_PREFIX)/lib -Wl,-rpath,$(LLVM_PREFIX)/lib -O3
else
LDFLAGS ?= $(COMMON_LDFLAGS) -L$(LLVM_PREFIX)/lib -Wl,-rpath,$(LLVM_PREFIX)/lib -O3
endif

endif

# Debug override
ifeq ($(DEBUG),1)
CXXFLAGS := $(filter-out -O3 -O2 -O1 -O0,$(CXXFLAGS)) -O0
CXXFLAGS += -fno-omit-frame-pointer -fno-pie
LDFLAGS := $(filter-out -O3 -O2 -O1 -O0,$(LDFLAGS)) -O0
LDFLAGS += -no-pie -rdynamic
endif

ifeq ($(STATIC),1)
LDFLAGS += -static -lc++ -lc++abi -lunwind -ldl -pthread
endif

export CC
export CXX
export CLANG_SCAN_DEPS
export CXXFLAGS
export LDFLAGS
export OS
export LLVM_PREFIX
export STD_LLVM_PREFIX

endif # ($(MAKELEVEL),0)

