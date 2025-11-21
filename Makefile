###############################################################################
# Project Configuration
###############################################################################

# Note: std module is always provided by libc++, so we don't build deps/std
# This project has no submodules

###############################################################################
# Compiler Configuration
###############################################################################

# Include shared compiler configuration
# Try local config first (standalone mode), then parent config (embedded mode)
-include config/compiler.mk
-include ../../config/compiler.mk

###############################################################################
# Build Configuration
###############################################################################

# Detect standalone vs parent usage via MAKELEVEL
# MAKELEVEL == 0 → invoked directly (standalone)
# MAKELEVEL  > 0 → invoked by parent make
STANDALONE := $(if $(filter 0,$(MAKELEVEL)),yes,no)

# OS-specific build directory (same logic as main project)
lowercase_os := $(if $(OS),$(shell echo $(OS) | tr '[:upper:]' '[:lower:]'),unknown)
BUILD_DIR ?= build-$(lowercase_os)
export BUILD_DIR

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
libraries =

# Dependency files: header deps (.d) and module deps
header_deps = $(objects:.o=.d) $(example-objects:.o=.d)

# clang-scan-deps is required (comes with Clang 20+)
# Use CLANG_SCAN_DEPS from config/compiler.mk if available, otherwise derive from compiler
clang_scan_deps := $(if $(CLANG_SCAN_DEPS),$(CLANG_SCAN_DEPS),$(shell dirname "$(CXX)" 2>/dev/null)/clang-scan-deps)

# One big dependency file for the whole project (module dependencies)
module_depfile = $(moduledir)/modules.dep

# All source files for dependency scanning
all_sources = $(modules) $(sources) $(example-modules) $(example-sources) $(wildcard $(exampledir)/*.test.c++)

###############################################################################
# Build Flags
###############################################################################

# Add sourcedir to include path
CXXFLAGS += -I$(sourcedir)

# Use explicit module building - build std.pcm explicitly from libc++ source
PCMFLAGS_COMMON = -fno-implicit-modules
PCMFLAGS_COMMON += -fmodule-file=std=$(moduledir)/std.pcm
PCMFLAGS_COMMON += $(foreach M, $(modules) $(example-modules), -fmodule-file=$(subst -,:,$(basename $(notdir $(M))))=$(moduledir)/$(basename $(notdir $(M))).pcm)
PCMFLAGS_COMMON += -fprebuilt-module-path=$(moduledir)/
PCMFLAGS_PRECOMPILE =
PCMFLAGS = $(PCMFLAGS_COMMON)

# Export compiler settings
export CC CXX CXXFLAGS LDFLAGS LLVM_PREFIX

###############################################################################
# Build Rules
###############################################################################

.SUFFIXES:
.SUFFIXES: .deps .c++m .c++ .test.c++ .pcm .o .test.o .a
.PRECIOUS: $(moduledir)/%.pcm $(objectdir)/%.d $(module_depfile)

###############################################################################
# Directory creation
###############################################################################

dirs = $(moduledir) $(objectdir) $(librarydir) $(binarydir)
$(dirs):
	@mkdir -p $@

###############################################################################

# Build std.pcm explicitly from libc++ source
BUILTIN_STD_OBJECT = $(objectdir)/std.o

# Filter out include paths, sysroot, and stdlib flags (not needed with -nostdinc++ and explicit isystem)
# -stdlib=libc++ is unused when using -nostdinc++ with explicit -isystem
STD_MODULE_FLAGS = $(filter-out -I% -isysroot% -stdlib=libc++%,$(CXXFLAGS)) -Wno-reserved-module-identifier -fno-implicit-modules -fno-implicit-module-maps

# Get the full path to the actual compiler binary (resolve symlinks) to detect when it changes
# STD_LLVM_PREFIX is now defined in config/compiler.mk
# Resolve symlinks to get the actual binary that will be used
CXX_BIN := $(shell command -v $(CXX) 2>/dev/null || echo $(CXX))
CXX_PATH := $(shell python3 -c "import os, sys; print(os.path.realpath(sys.argv[1]))" $(CXX_BIN) 2>/dev/null || echo $(CXX_BIN))

$(moduledir)/std.pcm: $(STD_LLVM_PREFIX)/share/libc++/v1/std.cppm $(CXX_PATH) | $(moduledir)
	@mkdir -p $(moduledir)
	@echo "Precompiling std module from libc++ source with matching flags"
	$(CXX) $(STD_MODULE_FLAGS) -nostdinc++ -isystem $(STD_LLVM_PREFIX)/include/c++/v1 \
		$(STD_LLVM_PREFIX)/share/libc++/v1/std.cppm --precompile -o $(moduledir)/std.pcm

$(objectdir)/std.o: $(moduledir)/std.pcm | $(objectdir)
	@mkdir -p $(@D)
	@echo "Compiling std module implementation"
	$(CXX) -fPIC -fno-implicit-modules -fno-implicit-module-maps \
		-fmodule-file=std=$(moduledir)/std.pcm \
		$(moduledir)/std.pcm -c -o $(objectdir)/std.o

# Rule for module interface units (.c++m) → produces .pcm
$(moduledir)/%.pcm: $(sourcedir)/%.c++m $(moduledir)/std.pcm $(module_depfile) | $(moduledir)
	@mkdir -p $(@D) $(objectdir)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -o $@

$(moduledir)/%.pcm: $(exampledir)/%.c++m $(moduledir)/std.pcm $(module_depfile) | $(moduledir)
	@mkdir -p $(@D) $(objectdir)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -o $@

# Rule to compile .pcm to .o (module interface object file)
$(objectdir)/%.o: $(moduledir)/%.pcm | $(objectdir)
	@mkdir -p $(@D)
	$(CXX) -fPIC $(PCMFLAGS) $< -c -o $@

# Rule for implementation units (.c++) → produces .o
$(objectdir)/%.o: $(sourcedir)/%.c++ $(moduledir)/std.pcm $(module_depfile) | $(objectdir) $(moduledir)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -MD -MF $(@:.o=.d) -o $@

$(objectdir)/%.o: $(exampledir)/%.c++ $(moduledir)/std.pcm $(module_depfile) | $(objectdir) $(moduledir)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -MD -MF $(@:.o=.d) -o $@

# Rule for test units (.test.c++) → produces .o
$(objectdir)/%.test.o: $(exampledir)/%.test.c++ $(moduledir)/std.pcm $(module_depfile) | $(objectdir) $(moduledir)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) -c $< -MD -MF $(@:.o=.d) -o $@

# Library creation
$(library) : $(objects)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

# Executable linking
$(binarydir)/%: $(exampledir)/%.c++ $(example-objects) $(library) $(libraries) $(BUILTIN_STD_OBJECT) | $(binarydir)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $^ -o $@

$(binarydir)/tools/%: $(toolsdir)/%.c++ $(library) $(libraries) $(BUILTIN_STD_OBJECT) | $(binarydir)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $^ -o $@

$(test-target): $(library) $(libraries) $(BUILTIN_STD_OBJECT) | $(binarydir)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $(LDFLAGS) $(library) $(libraries) $(BUILTIN_STD_OBJECT) -o $@

###############################################################################
# Dependency Generation
###############################################################################

# First pass: generate the complete module dependency graph
# Generate one big dependency file for all sources using p1689 format
# Parse JSON output to extract module dependencies and create Makefile rules
# Run clang-scan-deps on each module file individually to ensure all modules are detected
$(module_depfile): $(all_sources) scripts/parse_module_deps.py | $(objectdir) $(moduledir)
	@echo "Generating module dependency graph..."
	@rm -f $@
	@for src in $(all_sources); do \
		$(clang_scan_deps) -format=p1689 \
		    -module-files-dir $(moduledir) \
		    -- $(CXX) $(CXXFLAGS) -fno-implicit-modules -fmodule-file=std=$(moduledir)/std.pcm -fprebuilt-module-path=$(moduledir)/ $$src 2>/dev/null | \
		python3 scripts/parse_module_deps.py $(moduledir) $(objectdir) $$src >> $@ || true; \
	done

# Include it so Make knows about all .pcm rules
-include $(module_depfile)

# Include generated header dependencies
-include $(header_deps)


###############################################################################
# Phony Targets
###############################################################################

.DEFAULT_GOAL = run_examples

.PHONY: deps
deps: $(header_deps) $(module_depfile)

.PHONY: module
module: $(dirs) $(module_depfile) $(moduledir)/std.pcm $(library)

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

# Targets that must run sequentially (not in parallel)
.NOTPARALLEL: clean mostlyclean

.PHONY: clean
clean: mostlyclean
	rm -rf $(binarydir) $(librarydir)

.PHONY: mostlyclean
mostlyclean:
	rm -rf $(objectdir) $(moduledir)
	rm -f $(moduledir)/*.pcm

.PHONY: dump
dump:
	$(foreach v, $(sort $(.VARIABLES)), $(if $(filter file,$(origin $(v))), $(info $(v)=$($(v)))))
	@echo ''
