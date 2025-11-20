# Inspired by https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html

###############################################################################
# Project Configuration
###############################################################################

# Base submodules list
# Note: std module is always provided by libc++, so we don't build deps/std
base_submodules =
submodules = $(base_submodules)

# Ensure clean targets are serialized
.NOTPARALLEL: clean mostlyclean

# Detect OS early if not already set (needed for BUILD_DIR)
ifndef OS
OS = $(shell uname -s)
endif

# Normalize OS name for build directory selection
lowercase_os := $(shell echo $(OS) | tr '[:upper:]' '[:lower:]')
BUILD_DIR ?= build-$(lowercase_os)
export BUILD_DIR

###############################################################################
# Compiler Configuration
###############################################################################

# Include shared compiler configuration
# Try local config first (standalone mode), then parent config (embedded mode)
-include config/compiler.mk
-include ../../config/compiler.mk

# If config/compiler.mk wasn't found (standalone mode), define compiler settings inline
ifeq ($(MAKELEVEL),0)
ifndef CC
# Compiler configuration for standalone builds
ifndef OS
OS = $(shell uname -s)
endif

COMMON_CXXFLAGS = -std=c++23 -stdlib=libc++ -pthread
COMMON_CXXFLAGS += -fexperimental-library -Wall -Wextra
COMMON_LDFLAGS = -lc++
AR = ar
ARFLAGS = rv

# Platform-specific compiler configuration
ifeq ($(OS),Linux)
# Debian/Ubuntu package installation paths
CC = clang-20
CXX = clang++-20
CXXFLAGS = $(COMMON_CXXFLAGS) -I/usr/lib/llvm-20/include/c++/v1 -O3
ARCH = $(shell uname -m)
LDFLAGS = $(COMMON_LDFLAGS) -L/usr/lib/$(ARCH)-linux-gnu -O3
endif

ifeq ($(OS),Darwin)
export PATH := /opt/homebrew/bin:$(PATH)
# Use Homebrew LLVM - prefer llvm@19, fallback to llvm
ifndef LLVM_PREFIX
LLVM_PREFIX := $(shell if [ -d /opt/homebrew/opt/llvm@19 ]; then echo "/opt/homebrew/opt/llvm@19"; elif [ -d /opt/homebrew/opt/llvm ]; then echo "/opt/homebrew/opt/llvm"; else echo ""; fi)
endif
ifeq ($(LLVM_PREFIX),)
$(error LLVM not found. Please install with: brew install llvm@19 or brew install llvm)
endif
CC = $(LLVM_PREFIX)/bin/clang
CXX = $(LLVM_PREFIX)/bin/clang++
CXXFLAGS = $(COMMON_CXXFLAGS) -I$(LLVM_PREFIX)/include/c++/v1 -O3
LDFLAGS = $(COMMON_LDFLAGS) -L$(LLVM_PREFIX)/lib -L$(LLVM_PREFIX)/lib/c++ -Wl,-rpath,$(LLVM_PREFIX)/lib/c++ -O3
endif

export CC
export CXX
export CXXFLAGS
export LDFLAGS
endif # ifndef CC
endif # ($(MAKELEVEL),0)

###############################################################################
# Build Configuration
###############################################################################

# Detect standalone vs parent usage via MAKELEVEL
# MAKELEVEL == 0 → invoked directly (standalone)
# MAKELEVEL  > 0 → invoked by parent make
STANDALONE := $(if $(filter 0,$(MAKELEVEL)),yes,no)

# Default PREFIX based on context
# If PREFIX is not set, use appropriate default for the mode
ifndef PREFIX
ifeq ($(STANDALONE),yes)
# Standalone mode: use OS-specific build directory
PREFIX = $(BUILD_DIR)
endif
endif

# Project and directory configuration
project = $(lastword $(notdir $(CURDIR)))
sourcedir = ./$(project)
exampledir = ./examples
toolsdir = ./tools
moduledir = $(PREFIX)/pcm
objectdir = $(PREFIX)/obj
librarydir = $(PREFIX)/lib
binarydir = $(PREFIX)/bin

dirs = $(moduledir) $(objectdir) $(librarydir) $(binarydir)
$(dirs):
	@mkdir -p $@

# Compute all standalone-dependent variables once based on STANDALONE
# Always use built-in std module from libc++, no need for std.pcm
STD_MODULE_PATH =
STD_MODULE_PREBUILT_PATHS =
ifeq ($(STANDALONE),yes)
# Standalone mode: submodules are in deps/ directory, all use same build directory
SUBMODULE_PREFIX = deps
SUBMODULE_PREFIX_ARG = ../../$(BUILD_DIR)
else
# Parent project mode: submodules are siblings in deps/, use parent's PREFIX
SUBMODULE_PREFIX = ..
# PREFIX is already set (either explicitly by parent or defaulted by caller)
# Use provided path for submodules to ensure shared outputs
SUBMODULE_PREFIX_ARG = $(PREFIX)
endif

###############################################################################
# File Discovery
###############################################################################

# Library files
modules = $(wildcard $(sourcedir)/*.c++m)
sources = $(wildcard $(sourcedir)/*.c++)
objects = $(modules:$(sourcedir)%.c++m=$(objectdir)%.o) $(sources:$(sourcedir)%.c++=$(objectdir)%.o)

# Example files
example-programs = example
example-targets = $(example-programs:%=$(binarydir)/%)
example-modules = $(wildcard $(exampledir)/*.c++m)
example-sources = $(filter-out $(example-programs:%=$(exampledir)/%.c++), $(wildcard $(exampledir)/*.c++))
example-objects = $(example-sources:$(exampledir)%.c++=$(objectdir)%.o) $(example-modules:$(exampledir)%.c++m=$(objectdir)%.o)

# Tool files
tool-sources = $(wildcard $(toolsdir)/*.c++)
tool-targets = $(tool-sources:$(toolsdir)/%.c++=$(binarydir)/tools/%)

# Test files
test-program = test_runner
test-target = $(test-program:%=$(binarydir)/%)
test-source = $(sourcedir)/$(test-program).c++
test-object = $(test-source:$(sourcedir)%.c++=$(objectdir)%.o)
test-sources = $(wildcard $(exampledir)/*.test.c++)
test-objects = $(test-sources:$(exampledir)/%.test.c++=$(objectdir)/%.test.o)

# Library and dependencies
library = $(addprefix $(librarydir)/, lib$(project).a)
# Submodule libraries use the same PREFIX path as submodules
ifeq ($(STANDALONE),yes)
libraries = $(submodules:%=$(librarydir)/lib%.a)
else
libraries = $(submodules:%=$(SUBMODULE_PREFIX_ARG)/lib/lib%.a)
endif

# Source directories for dependency generation
library-sourcedirs = $(sourcedir)
example-sourcedirs = $(exampledir)
sourcedirs = $(library-sourcedirs) $(example-sourcedirs)

###############################################################################
# Build Flags
###############################################################################

# Add sourcedir to include path
CXXFLAGS += -I$(sourcedir)

# Module compilation flags
# Always use built-in std module from libc++
# Add module map file and prebuilt module path if LLVM_PREFIX is available (from parent Makefile or environment)
PCMFLAGS = -fno-implicit-modules -fno-implicit-module-maps
PCMFLAGS += -fmodule-file=std=$(moduledir)/std.pcm
PCMFLAGS += -fprebuilt-module-path=$(moduledir)/
ifdef LLVM_PREFIX
PCMFLAGS += -fmodule-map-file=$(LLVM_PREFIX)/include/c++/v1/module.modulemap
else
# Try to detect LLVM_PREFIX from common locations
LLVM_PREFIX_DETECTED := $(shell if [ -d /usr/local/llvm/include/c++/v1 ]; then echo "/usr/local/llvm"; elif [ -d /opt/homebrew/opt/llvm@19/include/c++/v1 ]; then echo "/opt/homebrew/opt/llvm@19"; elif [ -d /opt/homebrew/opt/llvm/include/c++/v1 ]; then echo "/opt/homebrew/opt/llvm"; else echo ""; fi)
PCMFLAGS += $(if $(LLVM_PREFIX_DETECTED),-fmodule-map-file=$(LLVM_PREFIX_DETECTED)/include/c++/v1/module.modulemap)
endif
PCMFLAGS += $(foreach P, $(foreach M, $(modules) $(example-modules), $(basename $(notdir $(M)))), -fmodule-file=$(subst -,:,$(P))=$(moduledir)/$(P).pcm)

BUILTIN_STD_OBJECT = $(objectdir)/std.o

###############################################################################
# Build Rules
###############################################################################

.SUFFIXES:
.SUFFIXES: .deps .c++m .c++ .test.c++ .pcm .o .test.o .a
.PRECIOUS: $(objectdir)/%.deps $(moduledir)/%.pcm

# Module compilation: .c++m -> .pcm
$(moduledir)/%.pcm: $(sourcedir)/%.c++m $(moduledir)/std.pcm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -o $@

$(moduledir)/%.pcm: $(exampledir)/%.c++m $(moduledir)/std.pcm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -o $@

# Object compilation: .pcm -> .o
$(objectdir)/%.o: $(moduledir)/%.pcm
	@mkdir -p $(@D)
	$(CXX) $(PCMFLAGS) -c $< -o $@

# Build std module once for this project
# If std.pcm already exists (e.g., from parent build), skip building it
$(moduledir)/std.pcm: | $(moduledir)
	@mkdir -p $(moduledir)
	@if [ -f $(moduledir)/std.pcm ]; then \
		echo "Using existing std.pcm from parent build"; \
	elif [ -n "$(LLVM_PREFIX)" ] && [ -f $(LLVM_PREFIX)/share/libc++/v1/std.cppm ]; then \
		echo "Precompiling std module from $(LLVM_PREFIX)/share/libc++/v1/std.cppm"; \
		$(CXX) -std=c++23 -pthread -fPIC -fexperimental-library \
			-nostdinc++ -isystem $(LLVM_PREFIX)/include/c++/v1 \
			-fno-implicit-modules -fno-implicit-module-maps \
			-Wall -Wextra -Wno-reserved-module-identifier -g -O3 \
			$(LLVM_PREFIX)/share/libc++/v1/std.cppm --precompile -o $(moduledir)/std.pcm; \
	elif [ -f /usr/lib/llvm-20/share/libc++/v1/std.cppm ]; then \
		echo "Precompiling std module from /usr/lib/llvm-20/share/libc++/v1/std.cppm"; \
		$(CXX) -std=c++23 -pthread -fPIC -fexperimental-library \
			-nostdinc++ -isystem /usr/lib/llvm-20/include/c++/v1 \
			-fno-implicit-modules -fno-implicit-module-maps \
			-Wall -Wextra -Wno-reserved-module-identifier -g -O3 \
			/usr/lib/llvm-20/share/libc++/v1/std.cppm --precompile -o $(moduledir)/std.pcm; \
	else \
		echo "Error: std.cppm not found and std.pcm does not exist at $(moduledir)/std.pcm"; \
		if [ -n "$(LLVM_PREFIX)" ]; then \
			echo "  Checked: $(LLVM_PREFIX)/share/libc++/v1/std.cppm"; \
		fi; \
		echo "  Checked: /usr/lib/llvm-20/share/libc++/v1/std.cppm"; \
		echo "Please ensure LLVM with libc++ modules is installed or std.pcm is built by parent."; \
		exit 1; \
	fi

$(objectdir)/std.o: $(moduledir)/std.pcm | $(objectdir)
	@mkdir -p $(@D)
	@echo "Compiling std module implementation with initializer"
	$(CXX) -fPIC -fno-implicit-modules -fno-implicit-module-maps \
		-fmodule-file=std=$(moduledir)/std.pcm \
		$(moduledir)/std.pcm -c -o $(objectdir)/std.o

# Object compilation: .c++ -> .o
$(objectdir)/%.o: $(sourcedir)/%.c++ $(moduledir)/std.pcm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -o $@

$(objectdir)/%.o: $(exampledir)/%.c++ $(moduledir)/std.pcm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -o $@

$(objectdir)/%.test.o: $(exampledir)/%.test.c++ $(moduledir)/std.pcm
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -o $@

# Library creation
$(library) : $(objects)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

# Executable linking
$(binarydir)/%: $(exampledir)/%.c++ $(example-objects) $(library) $(libraries) $(BUILTIN_STD_OBJECT)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $^ -o $@

$(binarydir)/tools/%: $(toolsdir)/%.c++ $(library) $(libraries) $(BUILTIN_STD_OBJECT)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $^ -o $@

# Module objects needed by test files (all example modules)
test-module-objects = $(example-modules:$(exampledir)/%.c++m=$(objectdir)/%.o)

$(test-target): $(library) $(libraries) $(BUILTIN_STD_OBJECT) $(test-objects) $(test-module-objects)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $(library) $(libraries) $(BUILTIN_STD_OBJECT) $(test-objects) $(test-module-objects) -o $@

###############################################################################
# Dependency Generation
###############################################################################

# Separate dependencies: library-only for module target, examples for examples target
library-dependencies = $(foreach D, $(library-sourcedirs), $(objectdir)/$(D).deps)
example-dependencies = $(foreach D, $(example-sourcedirs), $(objectdir)/$(D).deps)
dependencies = $(library-dependencies) $(example-dependencies)

# Generate dependencies for modules and sources
# Filter out std.pcm dependencies since we always use built-in std module
define create_dependency_hierarchy
	grep -HE '^[ ]*module[ ]+([a-z_0-9][a-z_0-9:]*)' $(1)/*.c++ 2>/dev/null | sed -nE 's|.+/([a-z_0-9\.\-]+)\.c\+\+:[ ]*module[ ]+([a-z_0-9][a-z_0-9:]*)[ ]*;|$(objectdir)/\1.o: $(moduledir)/\2.pcm|p' >> $(2) || true; \
	grep -HE '^[ ]*export[ ]+module' $(1)/*.c++m 2>/dev/null | sed -nE 's|.+/([a-z_0-9\-]+)\.c\+\+m.+|$(objectdir)/\1.o: $(moduledir)/\1.pcm|p' >> $(2) || true; \
	grep -HE '^[ ]*export[ ]+import[ ]+([a-z_0-9][a-z_0-9:]*)' $(1)/*.c++m 2>/dev/null | sed -nE 's|.+/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9][a-z_0-9:]*)[ ]*;|$(moduledir)/\1.pcm: $(moduledir)/\2.pcm|p' | grep -v ':.*std\.pcm' >> $(2) || true; \
	grep -HE '^[ ]*export[ ]+[ ]*import[ ]+:([a-z_0-9:]+)' $(1)/*.c++m 2>/dev/null | sed -nE 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9:]+)[ ]*;|$(moduledir)/\1\2\3.pcm: $(moduledir)/\1\-\4.pcm|p' >> $(2) || true; \
	grep -HE '^[ ]*import[ ]+([a-z_0-9][a-z_0-9:]*)' $(1)/*.c++m 2>/dev/null | sed -nE 's|.+/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9][a-z_0-9:]*)[ ]*;|$(moduledir)/\1.pcm: $(moduledir)/\2.pcm|p' | grep -v ':.*std\.pcm' >> $(2) || true; \
	grep -HE '^[ ]*import[ ]+:([a-z_0-9:]+)' $(1)/*.c++m 2>/dev/null | sed -nE 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9:]+)[ ]*;|$(moduledir)/\1\2\3.pcm: $(moduledir)/\1\-\4.pcm|p' >> $(2) || true; \
	grep -HE '^[ ]*import[ ]+([a-z_0-9][a-z_0-9:]*)' $(1)/*.c++ 2>/dev/null | sed -nE 's|.+/([a-z_0-9\.\-]+)\.c\+\+:[ ]*import[ ]+([a-z_0-9][a-z_0-9:]*)[ ]*;|$(objectdir)/\1.o: $(moduledir)/\2.pcm|p' | grep -v 'std\.pcm' >> $(2) || true; \
	grep -HE '^[ ]*import[ ]+:([a-z_0-9:]+)' $(1)/*.c++ 2>/dev/null | sed -nE 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9\.]*)\.c\+\+:.*import[ ]+:([a-z_0-9:]+)[ ]*;|$(objectdir)/\1\2\3.o: $(moduledir)/\1\-\4.pcm|p' >> $(2) || true;
endef

$(library-dependencies): $(modules) $(sources) | $(objectdir)
	@mkdir -p $(@D)
	$(call create_dependency_hierarchy, ./$(basename $(@F)), $@)

$(example-dependencies): $(example-modules) $(example-sources) | $(objectdir)
	@mkdir -p $(@D)
	$(call create_dependency_hierarchy, ./$(basename $(@F)), $@)

# Include library dependencies for module target
-include $(library-dependencies)
# Include example dependencies so example builds pull in their modules
-include $(example-dependencies)

###############################################################################
# Submodule Rules
###############################################################################

$(foreach M, $(submodules), $(MAKE) -C $(SUBMODULE_PREFIX)/$(M) deps PREFIX=$(SUBMODULE_PREFIX_ARG)):

$(foreach M, $(submodules), $(moduledir)/$(M).pcm):
#	git submodule update --init --depth 1
	$(MAKE) -C $(SUBMODULE_PREFIX)/$(basename $(@F)) module PREFIX=$(SUBMODULE_PREFIX_ARG)

$(SUBMODULE_PREFIX_ARG)/pcm/%.pcm:
	@if [ "$*" = "std" ]; then \
		echo "Skipping deps/std build: using LLVM's built-in std module"; \
		mkdir -p $(@D); \
		touch $@; \
	else \
		$(MAKE) -C $(SUBMODULE_PREFIX)/$* module PREFIX=$(SUBMODULE_PREFIX_ARG); \
	fi

$(librarydir)/%.a:
#	git submodule update --init --depth 1
	$(MAKE) -C $(SUBMODULE_PREFIX)/$(subst lib,,$(basename $(@F))) module PREFIX=$(SUBMODULE_PREFIX_ARG)

$(SUBMODULE_PREFIX_ARG)/lib/lib%.a: $(SUBMODULE_PREFIX_ARG)/pcm/%.pcm
	@if [ "$*" = "std" ]; then \
		echo "Skipping deps/std build: using LLVM's built-in std module"; \
		mkdir -p $(@D); \
		touch $@; \
	else \
		$(MAKE) -C $(SUBMODULE_PREFIX)/$* module PREFIX=$(SUBMODULE_PREFIX_ARG); \
	fi

###############################################################################
# Phony Targets
###############################################################################

.DEFAULT_GOAL = run_examples

###############################################################################
# High-level targets
###############################################################################

.PHONY: help module all examples run_examples tools tests run_tests clean mostlyclean

help:
	@echo "Tester Submodule - Targets"
	@echo ""
	@echo "  make help          - Show this help message"
	@echo "  make module        - Build modules, library, and required submodules"
	@echo "  make all           - Alias for module"
	@echo "  make examples      - Build example binaries"
	@echo "  make run_examples  - Build + run examples"
	@echo "  make tools         - Build tooling binaries"
	@echo "  make tests         - Build unit test binary"
	@echo "  make run_tests     - Build and run unit tests (TEST_TAGS=... supported)"
	@echo "  make clean         - Remove all build outputs except std.pcm"
	@echo "  make mostlyclean   - Remove object files only"

module: $(moduledir)/std.pcm $(objectdir)/std.o \
        $(foreach M,$(submodules),$(moduledir)/$(M).pcm) \
        $(foreach M,$(submodules),$(SUBMODULE_PREFIX_ARG)/lib/lib$(M).a) \
        library-deps \
        $(library)

all: module

examples: all $(example-dependencies) $(example-targets)

run_examples: examples
	$(example-targets)

tools: all $(tool-targets)

tests: all $(test-target)

run_tests: tests
	$(test-target) $(TEST_TAGS)

clean: mostlyclean
	rm -rf $(binarydir) $(librarydir)
	@if [ -d $(moduledir) ]; then \
		find $(moduledir) -mindepth 1 ! -name 'std.pcm' -exec rm -rf {} +; \
	fi

mostlyclean:
	rm -rf $(objectdir)

###############################################################################
# Dependency targets
###############################################################################

.PHONY: deps library-deps

deps: $(library-dependencies) $(example-dependencies)

library-deps: $(library-dependencies)

.PHONY: dump
dump:
	$(foreach v, $(sort $(.VARIABLES)), $(if $(filter file,$(origin $(v))), $(info $(v)=$($(v)))))
	@echo ''
