.SUFFIXES:
.SUFFIXES: .c++ .c++m .impl.c++ .test.c++ .pcm .o .impl.o .test.o
.DEFAULT_GOAL = run_example

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
CXXFLAGS += -I$(sourcedir) -I$(includedir)
LDFLAGS += -fuse-ld=lld

PCMFLAGS += -fno-implicit-modules -fno-implicit-module-maps
PCMFLAGS += -fmodule-file=std=$(moduledir)/std.pcm
PCMFLAGS += $(foreach P, $(foreach M, $(modules) $(example-modules), $(basename $(notdir $(M)))), -fmodule-file=$(subst -,:,$(P))=$(moduledir)/$(P).pcm)
CXXFLAGS += $(PCMFLAGS)

export CC
export CXX
export CXXFLAGS
export LDFLAGS

endif # ($(MAKELEVEL),0)

# PREFIX = . # FIXME
sourcedir = tester
exampledir = examples
moduledir = pcm
includedir = include
objectdir = obj
librarydir = lib
binarydir = bin

project = $(lastword $(notdir $(CURDIR)))
library = $(addprefix $(librarydir)/, lib$(project).a)

test-program = tester_runner
test-target = $(test-program:%=$(binarydir)/%)
test-sources = $(wildcard $(sourcedir)/*test.c++)
test-objects = $(test-program:%=$(objectdir)/%.o) $(test-sources:$(sourcedir)%.c++=$(objectdir)%.o)

example-programs = example
example-targets = $(example-programs:%=$(binarydir)/%)
example-modules = $(wildcard $(exampledir)/*.c++m)
example-sources = $(filter-out $(example-programs:%=$(exampledir)/%.c++), $(wildcard $(exampledir)/*.c++))
example-objects = $(example-sources:$(exampledir)%.c++=$(objectdir)%.o) $(example-modules:$(exampledir)%.c++m=$(objectdir)%.o)

programs = tester_runner
targets = $(programs:%=$(binarydir)/%)
sources = $(filter-out $(programs:%=$(sourcedir)/%.c++) $(test-program:%=$(sourcedir)/%.c++) $(test-sources), $(wildcard $(sourcedir)/*.c++))
modules = $(wildcard $(sourcedir)/*.c++m)
objects = $(modules:$(sourcedir)%.c++m=$(objectdir)%.o) $(sources:$(sourcedir)%.c++=$(objectdir)%.o)

libraries = # $(addprefix $(librarydir)/, libxxx.a)

.PRECIOUS: ./obj/Makefile.deps $(moduledir)/%.pcm

dependencies = $(objectdir)/Makefile.deps

$(objectdir)/%.o: $(moduledir)/%.pcm
	@mkdir -p $(@D)
	$(CXX) $(PCMFLAGS) $< -c -o $@

$(moduledir)/%.pcm: $(sourcedir)/%.c++m
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< --precompile -c -o $@

$(objectdir)/%.impl.o: $(sourcedir)/%.impl.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -fmodule-file=$(basename $(basename $(@F)))=$(moduledir)/$(basename $(basename $(@F))).pcm -c -o $@

$(objectdir)/%.test.o: $(sourcedir)/%.test.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -c -o $@

$(objectdir)/%.o: $(sourcedir)/%.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -c -o $@

$(binarydir)/%: $(sourcedir)/%.c++ $(objects) $(libraries)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(test-target): $(objects) $(test-objects) $(libraries)
	@mkdir -p $(@D)
	$(CXX) $(LDFLAGS) $^ -o $@

$(librarydir)/%.a:
#	git submodule update --init --depth 10
	$(MAKE) -C $(subst lib,,$(basename $(@F))) module PREFIX=..

$(library) : $(objects)
	@mkdir -p $(@D)
	$(AR) $(ARFLAGS) $@ $^

$(moduledir)/%.pcm: $(exampledir)/%.c++m
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< --precompile -c -o $@

$(objectdir)/%.impl.o: $(exampledir)/%.impl.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -fmodule-file=$(basename $(basename $(@F)))=$(moduledir)/$(basename $(basename $(@F))).pcm -c -o $@

$(objectdir)/%.test.o: $(exampledir)/%.test.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -c -o $@

$(objectdir)/%.o: $(exampledir)/%.c++
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $< -c -o $@

$(binarydir)/%: $(exampledir)/%.c++ $(example-objects) $(library)
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(LDFLAGS) $^ -o $@

$(dependencies):
	@mkdir -p $(@D)
#c++m module wrapping headers etc.
	grep -HE '^[ ]*export[ ]+module' $(sourcedir)/*.c++m | sed -E 's/.+\/([a-z_0-9\-]+)\.c\+\+m.+/$(objectdir)\/\1.o: $(moduledir)\/\1.pcm/' > $(dependencies)
#c++m module interface unit
	grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $(sourcedir)/*.c++m | sed -E 's/.+\/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9]+)[ ]*;/$(moduledir)\/\1.pcm: $(moduledir)\/\2.pcm/' >> $(dependencies)
#c++m module partition unit
	grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $(sourcedir)/*.c++m | sed -E 's/.+\/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9]+)[ ]*;/$(moduledir)\/\1\2\3.pcm: $(moduledir)\/\1\-\4.pcm/' >> $(dependencies)
	grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $(sourcedir)/*.c++ | sed -E 's/.+\/([a-z_0-9]+)(\-*)([a-z_0-9\.]*)\.c\+\+:.*import[ ]+:([a-z_0-9]+)[ ]*;/$(objectdir)\/\1\2\3.o: $(moduledir)\/\1\-\4.pcm/' >> $(dependencies)
#c++m module impl unit
	grep -HE '^[ ]*module[ ]+([a-z_0-9]+)' $(sourcedir)/*.c++ | sed -E 's/.+\/([a-z_0-9\.\-]+)\.c\+\+:[ ]*module[ ]+([a-z_0-9]+)[ ]*;/$(objectdir)\/\1.o: $(moduledir)\/\2.pcm/' >> $(dependencies)
#c++ source code
	grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $(sourcedir)/*.c++ | sed -E 's/.+\/([a-z_0-9\.\-]+)\.c\+\+:[ ]*import[ ]+([a-z_0-9]+)[ ]*;/$(objectdir)\/\1.o: $(moduledir)\/\2.pcm/' >> $(dependencies)
#c++m module wrapping headers etc.
	grep -HE '^[ ]*export[ ]+module' $(exampledir)/*.c++m | sed -E 's/.+\/([a-z_0-9\-]+)\.c\+\+m.+/$(objectdir)\/\1.o: $(moduledir)\/\1.pcm/' > $(dependencies)
#c++m module interface unit
	grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $(exampledir)/*.c++m | sed -E 's/.+\/([a-z_0-9\-]+)\.c\+\+m:[ ]*import[ ]+([a-z_0-9]+)[ ]*;/$(moduledir)\/\1.pcm: $(moduledir)\/\2.pcm/' >> $(dependencies)
#c++m module partition unit
	grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $(exampledir)/*.c++m | sed -E 's/.+\/([a-z_0-9]+)(\-*)([a-z_0-9]*)\.c\+\+m:.*import[ ]+:([a-z_0-9]+)[ ]*;/$(moduledir)\/\1\2\3.pcm: $(moduledir)\/\1\-\4.pcm/' >> $(dependencies)
	grep -HE '^[ ]*import[ ]+:([a-z_0-9]+)' $(exampledir)/*.c++ | sed -E 's/.+\/([a-z_0-9]+)(\-*)([a-z_0-9\.]*)\.c\+\+:.*import[ ]+:([a-z_0-9]+)[ ]*;/$(objectdir)\/\1\2\3.o: $(moduledir)\/\1\-\4.pcm/' >> $(dependencies)
#c++m module impl unit
	grep -HE '^[ ]*module[ ]+([a-z_0-9]+)' $(exampledir)/*.c++ | sed -E 's/.+\/([a-z_0-9\.\-]+)\.c\+\+:[ ]*module[ ]+([a-z_0-9]+)[ ]*;/$(objectdir)\/\1.o: $(moduledir)\/\2.pcm/' >> $(dependencies)
#c++ source code
	grep -HE '^[ ]*import[ ]+([a-z_0-9]+)' $(exampledir)/*.c++ | sed -E 's/.+\/([a-z_0-9\.\-]+)\.c\+\+:[ ]*import[ ]+([a-z_0-9]+)[ ]*;/$(objectdir)\/\1.o: $(moduledir)\/\2.pcm/' >> $(dependencies)

-include $(dependencies)

.PHONY: all
all: $(libraries) $(dependencies) $(targets)

.PHONY: lib
lib: $(library)

.PHONY: tests
tests: $(libraries) $(dependencies) $(test-target)

.PHONY: examples
examples: $(libraries) $(dependencies) $(example-targets)

.PHONY: run_example
run_example: examples
	$(example-targets)

.PHONY: clean
clean: mostlyclean
	rm -rf $(librarydir) $(includedir)

.PHONY: mostlyclean
mostlyclean:
	rm -rf $(objectdir) $(binarydir) $(moduledir)

.PHONY: dump
dump:
	$(foreach v, $(sort $(.VARIABLES)), $(if $(filter file,$(origin $(v))), $(info $(v)=$($(v)))))
	@echo ''