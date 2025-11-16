# Inspired by https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html

###############################################################################
# Project Configuration
###############################################################################

submodules = std

###############################################################################
# Compiler Configuration
###############################################################################

# Include shared compiler configuration (relative to project root)
# If used standalone, fall back to inline compiler configuration
-include ../../config/compiler.mk

# If config/compiler.mk wasn't found (standalone mode), define compiler settings inline
ifeq ($(MAKELEVEL),0)
ifndef CC
# Compiler configuration for standalone builds
ifndef OS
OS = $(shell uname -s)
endif

COMMON_CXXFLAGS = -std=c++23 -stdlib=libc++ -pthread
COMMON_CXXFLAGS += -fexperimental-library -Wall -Wextra -Wno-reserved-module-identifier -Wno-deprecated-declarations
COMMON_LDFLAGS = -lc++
AR = ar
ARFLAGS = rv

# Platform-specific compiler configuration
ifeq ($(OS),Linux)
# Debian/Ubuntu package installation paths
CC = clang-19
CXX = clang++-19
CXXFLAGS = $(COMMON_CXXFLAGS) -I/usr/include/c++/v1 -O3
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
# Standalone mode: use build/ directory
PREFIX = build
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

# Compute all standalone-dependent variables once based on STANDALONE
ifeq ($(STANDALONE),yes)
# Standalone mode: submodules are in deps/ directory, all use same build/ directory
SUBMODULE_PREFIX = deps
SUBMODULE_PREFIX_ARG = ../../build
STD_MODULE_PATH = $(moduledir)/std.pcm
STD_MODULE_PREBUILT_PATHS = $(moduledir)/
else
# Parent project mode: submodules are siblings in deps/, use parent's PREFIX
SUBMODULE_PREFIX = ..
# PREFIX is already set (either explicitly by parent or defaulted by caller)
# Use provided path for submodules to ensure shared outputs
SUBMODULE_PREFIX_ARG = $(PREFIX)
# Use the same PREFIX path that submodules use (SUBMODULE_PREFIX_ARG)
# This ensures std.pcm path matches where it's actually built
STD_MODULE_PATH = $(SUBMODULE_PREFIX_ARG)/pcm/std.pcm
STD_MODULE_PREBUILT_PATHS = $(moduledir)/ $(SUBMODULE_PREFIX_ARG)/pcm/
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
PCMFLAGS = -fno-implicit-modules -fno-implicit-module-maps
PCMFLAGS += $(foreach P, $(foreach M, $(modules) $(example-modules), $(basename $(notdir $(M)))), -fmodule-file=$(subst -,:,$(P))=$(moduledir)/$(P).pcm)
PCMFLAGS += -fmodule-file=std=$(STD_MODULE_PATH)
PCMFLAGS += $(foreach P, $(STD_MODULE_PREBUILT_PATHS), -fprebuilt-module-path=$(P))
.PRECIOUS: $(STD_MODULE_PATH)

###############################################################################
# Build Rules
###############################################################################

.SUFFIXES:
.SUFFIXES: .deps .c++m .c++ .test.c++ .pcm .o .test.o .a
.PRECIOUS: $(objectdir)/%.deps $(moduledir)/%.pcm

# Module compilation: .c++m -> .pcm
$(moduledir)/%.pcm: $(sourcedir)/%.c++m
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -o $@

$(moduledir)/%.pcm: $(exampledir)/%.c++m
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -o $@

# Object compilation: .pcm -> .o
$(objectdir)/%.o: $(moduledir)/%.pcm
	@mkdir -p $(@D)
	$(CXX) $(PCMFLAGS) -c $< -o $@

# Object compilation: .c++ -> .o
$(objectdir)/%.o: $(sourcedir)/%.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -o $@

$(objectdir)/%.o: $(exampledir)/%.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -o $@

$(objectdir)/%.test.o: $(exampledir)/%.test.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -o $@

# Library creation
$(library) : $(objects)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

# Executable linking
$(binarydir)/%: $(exampledir)/%.c++ $(example-objects) $(library) $(libraries)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $^ -o $@

$(binarydir)/tools/%: $(toolsdir)/%.c++ $(library) $(libraries)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $^ -o $@

$(test-target): $(library) $(libraries)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $(library) $(libraries) -o $@

###############################################################################
# Dependency Generation
###############################################################################

# Separate dependencies: library-only for module target, examples for examples target
library-dependencies = $(foreach D, $(library-sourcedirs), $(objectdir)/$(D).deps)
example-dependencies = $(foreach D, $(example-sourcedirs), $(objectdir)/$(D).deps)
dependencies = $(library-dependencies) $(example-dependencies)

define create_dependency_hierarchy
	-grep -HE '^[ ]*export[ ]+module' $(1)/*.c++m | sed -E 's|.+/([a-z_0-9\-]+)\.c\+\+m.+|$(objectdir)/\1.o: $(moduledir)/\1.pcm|' >> $(2)
	-grep -HE '^[ ]*export[ ]+import[ ]+([a-z_0-9]+)' $(1)/*.c++m | sed -E 's|.+/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9]+)[ ]*;|$(moduledir)/\1.pcm: $(moduledir)/\2.pcm|' >> $(2)
	-grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $(1)/*.c++m | sed -E 's|.+/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9]+)[ ]*;|$(moduledir)/\1.pcm: $(moduledir)/\2.pcm|' >> $(2)
	-grep -HE '^[ ]*export[ ]+[ ]*import[ ]+:([a-z_0-9]+)' $(1)/*.c++m | sed -E 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9]+)[ ]*;|$(moduledir)/\1\2\3.pcm: $(moduledir)/\1\-\4.pcm|' >> $(2)
	-grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $(1)/*.c++m | sed -E 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9]+)[ ]*;|$(moduledir)/\1\2\3.pcm: $(moduledir)/\1\-\4.pcm|' >> $(2)
	grep -HE '^[ ]*module[ ]+([a-z_0-9]+)' $(1)/*.c++ | sed -E 's|.+/([a-z_0-9\.\-]+)\.c\+\+:[ ]*module[ ]+([a-z_0-9]+)[ ]*;|$(objectdir)/\1.o: $(moduledir)/\2.pcm|' >> $(2)
	grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $(1)/*.c++ | sed -E 's|.+/([a-z_0-9\.\-]+)\.c\+\+:[ ]*import[ ]+([a-z_0-9]+)[ ]*;|$(objectdir)/\1.o: $(moduledir)/\2.pcm|' >> $(2)
	grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $(1)/*.c++ | sed -E 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9\.]*)\.c\+\+:.*import[ ]+:([a-z_0-9]+)[ ]*;|$(objectdir)/\1\2\3.o: $(moduledir)/\1\-\4.pcm|' >> $(2)
endef

$(library-dependencies): $(modules) $(sources)
	@mkdir -p $(@D)
	$(call create_dependency_hierarchy, ./$(basename $(@F)), $@)

$(example-dependencies): $(example-modules) $(example-sources)
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
	$(MAKE) -C $(SUBMODULE_PREFIX)/$* module PREFIX=$(SUBMODULE_PREFIX_ARG)

$(librarydir)/%.a:
#	git submodule update --init --depth 1
	$(MAKE) -C $(SUBMODULE_PREFIX)/$(subst lib,,$(basename $(@F))) module PREFIX=$(SUBMODULE_PREFIX_ARG)

$(SUBMODULE_PREFIX_ARG)/lib/lib%.a: $(SUBMODULE_PREFIX_ARG)/pcm/%.pcm
	$(MAKE) -C $(SUBMODULE_PREFIX)/$* module PREFIX=$(SUBMODULE_PREFIX_ARG)

###############################################################################
# Phony Targets
###############################################################################

.DEFAULT_GOAL = run_examples

.PHONY: deps
deps: $(library-dependencies) $(example-dependencies)

.PHONY: library-deps
library-deps: $(library-dependencies)

.PHONY: module
module: $(foreach M,$(submodules),$(moduledir)/$(M).pcm) \
        $(foreach M,$(submodules),$(SUBMODULE_PREFIX_ARG)/lib/lib$(M).a) \
        library-deps \
        $(library)

.PHONY: all
all: module

.PHONY: examples
examples: all $(example-dependencies) $(example-targets)

.PHONY: tools
tools: all $(tool-targets)

.PHONY: run_examples
run_examples: examples
	$(example-targets)

.PHONY: tests
tests: all $(test-target)

.PHONY: run_tests
run_tests: tests
	$(test-target) $(TEST_TAGS)

.PHONY: clean
clean: mostlyclean
	rm -rf $(binarydir) $(librarydir)

.PHONY: mostlyclean
mostlyclean:
	rm -rf $(objectdir) $(moduledir)

.PHONY: dump
dump:
	$(foreach v, $(sort $(.VARIABLES)), $(if $(filter file,$(origin $(v))), $(info $(v)=$($(v)))))
	@echo ''
