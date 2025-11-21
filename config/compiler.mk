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
ifeq ($(OS),Linux)
# Debian/Ubuntu package installation paths
CC = clang-20
CXX = clang++-20
CXXFLAGS = $(COMMON_CXXFLAGS) -I/usr/lib/llvm-20/include/c++/v1 -O3
ARCH = $(shell uname -m)
LDFLAGS = $(COMMON_LDFLAGS) -L/usr/lib/$(ARCH)-linux-gnu -lc++ -O3
endif

ifeq ($(OS),Darwin)
# Assume natively built Clang 20+ at /usr/local/llvm with libc++
# You can override by setting LLVM_PREFIX environment variable
ifndef LLVM_PREFIX
LLVM_PREFIX := /usr/local/llvm
endif
ifeq ($(wildcard $(LLVM_PREFIX)/bin/clang++),)
$(error LLVM not found at $(LLVM_PREFIX). Please build Clang 20+ natively and install to /usr/local/llvm)
endif
# Force LLVM unless the user explicitly overrides on the command line.
# This avoids picking up system 'cc'/'c++' from the environment.
override CC := $(LLVM_PREFIX)/bin/clang
override CXX := $(LLVM_PREFIX)/bin/clang++
# Resolve an SDK once to avoid mixing CommandLineTools and Xcode paths
SDKROOT ?= $(shell xcrun --show-sdk-path 2>/dev/null)
export SDKROOT

# Assume LLVM has libc++ (natively built Clang includes libc++)
override CXXFLAGS := $(COMMON_CXXFLAGS) -nostdinc++ -isystem $(LLVM_PREFIX)/include/c++/v1 -fno-implicit-modules -O3
# Ensure we consistently use one SDK for C headers/libs and link against LLVM libc++
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
export CXXFLAGS
export LDFLAGS
export OS
export LLVM_PREFIX

endif # ($(MAKELEVEL),0)

