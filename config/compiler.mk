# Shared Compiler Configuration
# Include this file in all Makefiles to ensure consistent compiler settings

ifeq ($(MAKELEVEL),0)

ifndef OS
OS = $(shell uname -s)
endif

COMMON_CXXFLAGS = -std=c++23 -stdlib=libc++ -pthread -fPIC
COMMON_CXXFLAGS += -fexperimental-library -Wall -Wextra -Wno-reserved-module-identifier -Wno-deprecated-declarations -g
COMMON_LDFLAGS =
AR = ar
ARFLAGS = rv

# Platform-specific compiler configuration
ifeq ($(OS),Linux)
# Debian/Ubuntu package installation paths
CC = clang-19
CXX = clang++-19
CXXFLAGS = $(COMMON_CXXFLAGS) -I/usr/include/c++/v1 -O3
ARCH = $(shell uname -m)
LDFLAGS = $(COMMON_LDFLAGS) -L/usr/lib/$(ARCH)-linux-gnu -lc++ -O3
endif

ifeq ($(OS),Darwin)
export PATH := /opt/homebrew/bin:$(PATH)
# Use Homebrew LLVM from the unversioned symlink at /opt/homebrew/opt/llvm.
# Note: System clang from Xcode doesn't fully support C++23 modules, so Homebrew LLVM is required.
# You can override by setting LLVM_PREFIX environment variable
ifndef LLVM_PREFIX
LLVM_PREFIX := $(shell if [ -d /opt/homebrew/opt/llvm ]; then echo "/opt/homebrew/opt/llvm"; else echo ""; fi)
endif
ifeq ($(LLVM_PREFIX),)
$(error LLVM not found. Please install with: brew install llvm)
endif
# Force Homebrew LLVM unless the user explicitly overrides on the command line.
# This avoids picking up system 'cc'/'c++' from the environment.
override CC := $(LLVM_PREFIX)/bin/clang
override CXX := $(LLVM_PREFIX)/bin/clang++
# Resolve an SDK once to avoid mixing CommandLineTools and Xcode paths
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
export SDKROOT
# Prevent mixing Apple libc++ headers with Homebrew libc++: rely on Homebrew's c++ headers only
CXXFLAGS ?= $(COMMON_CXXFLAGS) -nostdinc++ -isystem $(LLVM_PREFIX)/include/c++/v1 -O3
# Ensure we consistently use one SDK for C headers/libs and link against Homebrew libc++
# Critical: Explicit libc++ linkage with rpath to fix exception unwinding issues
# This ensures the correct libc++ is used at link and runtime, resolving uncaught exceptions
ifneq ($(SDKROOT),)
CXXFLAGS += -isysroot $(SDKROOT)
LDFLAGS ?= $(COMMON_LDFLAGS) -isysroot $(SDKROOT) -L$(LLVM_PREFIX)/lib/c++ -L$(LLVM_PREFIX)/lib -Wl,-rpath,$(LLVM_PREFIX)/lib/c++ -Wl,-rpath,$(LLVM_PREFIX)/lib -lc++ -O3
else
LDFLAGS ?= $(COMMON_LDFLAGS) -L$(LLVM_PREFIX)/lib/c++ -L$(LLVM_PREFIX)/lib -Wl,-rpath,$(LLVM_PREFIX)/lib/c++ -Wl,-rpath,$(LLVM_PREFIX)/lib -lc++ -O3
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
export CXXFLAGS
export LDFLAGS
export OS

endif # ($(MAKELEVEL),0)

