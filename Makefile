# Inspired by https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1204r0.html

###############################################################################

submodules = std

###############################################################################

.SUFFIXES:
.SUFFIXES: .c++m .c++ .test.c++ .pcm .o .test.o
.DEFAULT_GOAL = run_examples

ifeq ($(MAKELEVEL),0)

ifndef OS
OS = $(shell uname -s)
endif

ifeq ($(OS),Linux)
CC = /usr/lib/llvm-18/bin/clang
CXX = /usr/lib/llvm-18/bin/clang++
CXXFLAGS = -pthread -I/usr/lib/llvm-18/include/c++/v1
LDFLAGS = -lc++ -L/usr/lib/llvm-18/lib/c++
endif

ifeq ($(OS),Darwin)
CC = /opt/homebrew/opt/llvm/bin/clang
CXX = /opt/homebrew/opt/llvm/bin/clang++
CXXFLAGS =-I/opt/homebrew/opt/llvm/include/c++/v1 -Ofast
LDFLAGS = -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++ -Ofast
endif

ifeq ($(OS),Github)
CC = /usr/local/opt/llvm/bin/clang
CXX = /usr/local/opt/llvm/bin/clang++
CXXFLAGS = -I/usr/local/opt/llvm/include/ -I/usr/local/opt/llvm/include/c++/v1
LDFLAGS = -L/usr/local/opt/llvm/lib/c++ -Wl,-rpath,/usr/local/opt/llvm/lib/c++
endif

CXXFLAGS += -std=c++23 -stdlib=libc++
CXXFLAGS += -Wall -Wextra -Wno-reserved-module-identifier -Wno-deprecated-declarations
CXXFLAGS += -I$(sourcedir)
LDFLAGS += -fuse-ld=lld

export CC
export CXX
export CXXFLAGS
export LDFLAGS

endif # ($(MAKELEVEL),0)

PCMFLAGS = -fno-implicit-modules -fno-implicit-module-maps
PCMFLAGS += $(foreach P, $(foreach M, $(modules) $(example-modules), $(basename $(notdir $(M)))), -fmodule-file=$(subst -,:,$(P))=$(moduledir)/$(P).pcm)
PCMFLAGS += -fmodule-file=std=$(moduledir)/std.pcm

###############################################################################

project = $(lastword $(notdir $(CURDIR)))
library = $(addprefix $(librarydir)/, lib$(project).a)

PREFIX = .
sourcedir = ./$(project)
exampledir = ./examples
sourcedirs = $(sourcedir) $(exampledir)
moduledir = $(PREFIX)/pcm
objectdir = $(PREFIX)/obj
librarydir = $(PREFIX)/lib
binarydir = $(PREFIX)/bin

modules = $(wildcard $(sourcedir)/*.c++m)
sources = $(wildcard $(sourcedir)/*.c++)
objects = $(modules:$(sourcedir)%.c++m=$(objectdir)%.o) $(sources:$(sourcedir)%.c++=$(objectdir)%.o)

example-programs = example
example-targets = $(example-programs:%=$(binarydir)/%)
example-modules = $(wildcard $(exampledir)/*.c++m)
example-sources = $(filter-out $(example-programs:%=$(exampledir)/%.c++), $(wildcard $(exampledir)/*.c++))
example-objects = $(example-sources:$(exampledir)%.c++=$(objectdir)%.o) $(example-modules:$(exampledir)%.c++m=$(objectdir)%.o)

###############################################################################

.PRECIOUS: %.deps $(moduledir)/%.pcm

###############################################################################

$(foreach P, $(submodules), $(moduledir)/$(P).pcm):
#	git submodule update --init --depth 1
	$(MAKE) -C $(subst lib,,$(basename $(@F))) module PREFIX=..

$(librarydir)/%.a:
#	git submodule update --init --depth 1
	$(MAKE) -C $(subst lib,,$(basename $(@F))) module PREFIX=..

###############################################################################

$(objectdir)/%.o: $(moduledir)/%.pcm
	@mkdir -p $(@D)
	$(CXX) $(PCMFLAGS) $< -c -o $@

$(moduledir)/%.pcm: $(sourcedir)/%.c++m
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< --precompile -c -o $@

$(objectdir)/%.o: $(sourcedir)/%.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< -c -o $@

$(library) : $(objects)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(moduledir)/%.pcm: $(exampledir)/%.c++m
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< --precompile -c -o $@

$(objectdir)/%.test.o: $(exampledir)/%.test.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -c -o $@

$(objectdir)/%.o: $(exampledir)/%.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(PCMFLAGS) $< -c -o $@

$(binarydir)/%: $(exampledir)/%.c++ $(example-objects) $(library)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

###############################################################################

dependencies = ./Makefile.deps

.PHONY: $(temp)

temp = $(addprefix deps_,$(sourcedirs))

.PHONY: deps

deps: $(temp)
	@cat $(dependencies)

$(temp): deps_%:
	@mkdir -p $(objectdir)
	@grep -HE '^[ ]*export[ ]+module' $*/*.c++m | sed -E 's|.+/([a-z_0-9\-]+)\.c\+\+m.+|$(objectdir)/\1.o: $(moduledir)/\1.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*export[ ]+import[ ]+([a-z_0-9]+)' $*/*.c++m | sed -E 's|.+/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9]+)[ ]*;|$(moduledir)/\1.pcm: $(moduledir)/\2.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $*/*.c++m | sed -E 's|.+/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9]+)[ ]*;|$(moduledir)/\1.pcm: $(moduledir)/\2.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*export[ ]+[ ]*import[ ]+:([a-z_0-9]+)' $*/*.c++m | sed -E 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9]+)[ ]*;|$(moduledir)/\1\2\3.pcm: $(moduledir)/\1\-\4.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $*/*.c++m | sed -E 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9]+)[ ]*;|$(moduledir)/\1\2\3.pcm: $(moduledir)/\1\-\4.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*module[ ]+([a-z_0-9]+)' $*/*.c++ | sed -E 's|.+/([a-z_0-9\.\-]+)\.c\+\+:[ ]*module[ ]+([a-z_0-9]+)[ ]*;|$(objectdir)/\1.o: $(moduledir)/\2.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $*/*.c++ | sed -E 's|.+/([a-z_0-9\.\-]+)\.c\+\+:[ ]*import[ ]+([a-z_0-9]+)[ ]*;|$(objectdir)/\1.o: $(moduledir)/\2.pcm|' >> $(dependencies)
	@grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $*/*.c++ | sed -E 's|.+/([a-z_0-9]+)(\-*)([a-z_0-9\.]*)\.c\+\+:.*import[ ]+:([a-z_0-9]+)[ ]*;|$(objectdir)/\1\2\3.o: $(moduledir)/\1\-\4.pcm|'|  >> $(dependencies)

###############################################################################

.PHONY: all
all: module

.PHONY: module
module: deps $(library)

.PHONY: examples
examples: all $(example-targets)

.PHONY: run_examples
run_examples: examples
	$(example-targets)

.PHONY: clean
clean: mostlyclean
	rm -rf $(binarydir) $(librarydir)

.PHONY: mostlyclean
mostlyclean:
	rm -rf $(objectdir) $(moduledir) Makefile.deps

.PHONY: dump
dump:
	$(foreach v, $(sort $(.VARIABLES)), $(if $(filter file,$(origin $(v))), $(info $(v)=$($(v)))))
	@echo ''

###############################################################################

-include $(dependencies)
