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

# Detect problematic Clang exception handling on macOS ARM (see llvm/llvm-project#92121).
# Root cause: LLVM's built-in unwinder enabled by default in Homebrew Clang 18+ on macOS ARM
# causes exceptions thrown from module implementations or constructors in return statements
# to not be caught properly. This is specific to macOS ARM due to Darwin's exception model
# and doesn't occur on Linux (which uses libunwind/libgcc instead).
#
# The proper fix requires rebuilding LLVM with:
#   -DLIBCXXABI_USE_LLVM_UNWINDER=OFF -DCOMPILER_RT_USE_LLVM_UNWINDER=OFF
# Since we're using Homebrew's pre-built LLVM, we work around with:
# 1. Explicit libc++ linkage (already done above)
# 2. Conservative optimization when CLANG_EXCEPTIONS_WORKAROUND=1
CLANG_VERSION_MAJOR := $(shell "$(CXX)" --version 2>/dev/null | sed -nE 's/.*clang version ([0-9]+).*/\1/p' | head -n1)
ifdef CLANG_EXCEPTIONS_WORKAROUND
# Mitigation: disable optimization to reduce unwinder-related miscompilation.
# This workaround applies to all Clang versions when explicitly enabled.
# NOTE: This is a temporary workaround. The proper fix requires rebuilding LLVM
# without the built-in unwinder, or waiting for an upstream fix.
CXXFLAGS := $(filter-out -O3 -O2 -O1 -O0,$(CXXFLAGS)) -O0 -fno-inline-functions -fno-vectorize -fno-slp-vectorize
CXXFLAGS += -frtti -fexceptions -fcxx-exceptions
LDFLAGS := $(filter-out -O3 -O2 -O1 -O0,$(LDFLAGS)) -O0
$(warning Applied exception handling workaround for Clang $(CLANG_VERSION_MAJOR) (llvm/llvm-project#92121) - using -O0)
else
# Error out on incompatible Clang versions (18+) unless workaround is explicitly enabled
ifneq ($(shell test $(CLANG_VERSION_MAJOR) -ge 18 && echo yes),)
$(error \
    Detected Clang $(CLANG_VERSION_MAJOR) on macOS ARM, which is incompatible due to LLVM built-in unwinder bug (llvm/llvm-project#92121). \
    Exceptions cannot be caught properly, causing uncaught exception crashes. \
    \
    Solutions: \
    1. Build with workaround: CLANG_EXCEPTIONS_WORKAROUND=1 make \
    2. Rebuild LLVM without built-in unwinder: \
       brew install llvm --build-from-source with -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
    3. Use an older Clang version (< 18) or wait for upstream fix \
    \
    See: https://github.com/llvm/llvm-project/issues/92121)
endif
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

