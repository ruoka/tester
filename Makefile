# Inspired by https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html

###############################################################################
# Project Configuration
###############################################################################

# Note: std module is always provided by libc++, so we don't build deps/std
submodules =

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

# Library and dependencies
library = $(addprefix $(librarydir)/, lib$(project).a)
# Submodule libraries use the same PREFIX path as submodules
ifeq ($(STANDALONE),yes)
libraries = $(submodules:%=$(librarydir)/lib%.a)
else
libraries = $(submodules:%=$(SUBMODULE_PREFIX_ARG)/lib/lib%.a)
endif

# Dependency files: header deps (.d) and module deps
header_deps = $(objects:.o=.d) $(example-objects:.o=.d)

# clang-scan-deps is required (comes with Clang 20+)
# Use the same directory as the compiler
clang_scan_deps := $(shell dirname "$(CXX)" 2>/dev/null)/clang-scan-deps

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

# Export compiler settings for submodules
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

$(moduledir)/std.pcm: | $(moduledir)
	@echo "Precompiling std module from $(LLVM_PREFIX)/share/libc++/v1/std.cppm"
	$(CXX) -std=c++23 -pthread -fPIC -fexperimental-library \
		-nostdinc++ -isystem $(LLVM_PREFIX)/include/c++/v1 \
		-fno-implicit-modules -fno-implicit-module-maps \
		-Wall -Wextra -Wno-reserved-module-identifier -g -O3 \
		$(LLVM_PREFIX)/share/libc++/v1/std.cppm --precompile -o $(moduledir)/std.pcm

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
	@for src in $(modules) $(example-modules); do \
		$(clang_scan_deps) -format=p1689 \
		    -module-files-dir $(moduledir) \
		    -- $(CXX) $(CXXFLAGS) -fno-implicit-modules -fmodule-file=std=$(moduledir)/std.pcm -fprebuilt-module-path=$(moduledir)/ $$src 2>/dev/null | \
		python3 scripts/parse_module_deps.py $(moduledir) $(objectdir) >> $@ || true; \
	done
	@for src in $(sources) $(example-sources) $(wildcard $(exampledir)/*.test.c++); do \
		$(clang_scan_deps) -format=p1689 \
		    -module-files-dir $(moduledir) \
		    -- $(CXX) $(CXXFLAGS) -fno-implicit-modules -fmodule-file=std=$(moduledir)/std.pcm -fprebuilt-module-path=$(moduledir)/ $$src 2>/dev/null | \
		python3 scripts/parse_module_deps.py $(moduledir) $(objectdir) >> $@ || true; \
	done

# Include it so Make knows about all .pcm rules
-include $(module_depfile)

# Include generated header dependencies
-include $(header_deps)

###############################################################################
# Submodule Rules
###############################################################################

# Submodules build into PREFIX/pcm and PREFIX/lib via SUBMAKE_PREFIX_ARG
# Since moduledir = $(PREFIX)/pcm and librarydir = $(PREFIX)/lib, no copy needed
$(submodules:%=$(moduledir)/%.pcm): $(moduledir)/%.pcm: | $(moduledir)
	@:

$(librarydir)/lib%.a: | $(librarydir)
	@mkdir -p $(librarydir)
	$(MAKE) -C $(SUBMODULE_PREFIX)/$* lib PREFIX=$(SUBMODULE_PREFIX_ARG)

###############################################################################
# Phony Targets
###############################################################################

.DEFAULT_GOAL = run_examples

.PHONY: deps
deps: $(header_deps) $(module_depfile)

.PHONY: module
module: $(moduledir)/std.pcm $(foreach M,$(submodules),$(moduledir)/$(M).pcm) \
        $(foreach M,$(submodules),$(SUBMODULE_PREFIX_ARG)/lib/lib$(M).a) \
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
	rm -f $(moduledir)/*.pcm

.PHONY: dump
dump:
	$(foreach v, $(sort $(.VARIABLES)), $(if $(filter file,$(origin $(v))), $(info $(v)=$($(v)))))
	@echo ''
