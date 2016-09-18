#
# Configuration variables
#

EXECUTABLE        = nesalizer
# Separate build directory
BUILD_DIR         = build
ifeq ($(origin CXX), default)
  CXX             = g++
endif
ifeq ($(origin CC), default)
  CC              = gcc
endif
# Extra flags passed during compilation and linking
EXTRA             =
EXTRA_LINK        =
# "debug", "release", or "release-debug". "release-debug" adds debugging
# information in addition to optimizing.
CONF              = debug
# If "1", includes a simple debugger (see cpu.cpp) for internal use. Has
# readline dependency.
INCLUDE_DEBUGGER  = 0
# If "1", a movie is recorded to movie.mp4 using libav (movie.cpp)
RECORD_MOVIE      = 0
# If "1", passes -rdynamic to add symbols for backtraces
BACKTRACE_SUPPORT = 1
# If "1", configures for automatic test ROM running
TEST              = 0

# If V is "1", commands are printed as they are executed
ifneq ($(V),1)
    q = @
endif

# 1 if "clang" occurs in the output of CXX -v, otherwise 0
is_clang := $(if $(findstring clang,$(shell "$(CXX)" -v 2>&1)),1,0)

#
# Source files and libraries
#

cpp_sources = audio apu blip_buf common controller cpu input main md5   \
  mapper mapper_0 mapper_1 mapper_2 mapper_3 mapper_4 mapper_5 mapper_7 \
  mapper_9 mapper_10 mapper_11 mapper_13 mapper_28 mapper_71 mapper_232 \
  ppu rom save_states sdl_backend timing
# Use C99 for the handy designated initializers feature
c_sources = tables

ifeq ($(RECORD_MOVIE),1)
    cpp_sources += movie
endif
ifeq ($(TEST),1)
    cpp_sources += test
endif

cpp_objects = $(addprefix $(BUILD_DIR)/,$(cpp_sources:=.o))
c_objects   = $(addprefix $(BUILD_DIR)/,$(c_sources:=.o))
objects     = $(c_objects) $(cpp_objects)
deps        = $(addprefix $(BUILD_DIR)/,$(c_sources:=.d) $(cpp_sources:=.d))

LDLIBS := $(shell sdl2-config --libs) -lrt

ifeq ($(INCLUDE_DEBUGGER),1)
    LDLIBS += -lreadline
endif

ifeq ($(RECORD_MOVIE),1)
    LDLIBS += -lavcodec -lavformat -lavutil -lswscale
endif

#
# Optimizations and warnings
#

ifeq ($(is_clang),1)
    # Older Clang versions barf on some of the optimizations below
    optimizations = -O3 -ffast-math
else
    # Assume GCC
    optimizations = -Ofast -mfpmath=sse -funsafe-loop-optimizations
endif

optimizations += -msse3 -flto -fno-exceptions -DNDEBUG

warnings = -Wall -Wextra -Wdisabled-optimization -Wmissing-format-attribute \
  -Wno-switch -Wredundant-decls -Wuninitialized

ifeq ($(is_clang),0)
    # Not supported on Clang
    warnings += -Wmaybe-uninitialized -Wunsafe-loop-optimizations
endif

#
# Configuration
#

ifeq ($(filter debug release release-debug,$(CONF)),)
    $(error unknown configuration "$(CONF)")
else ifneq ($(MAKECMDGOALS),clean)
    # make will restart after updating the .d dependency files, so make sure we
    # only print this message once
    ifndef MAKE_RESTARTS
        $(info Using configuration "$(CONF)")
    endif
endif

ifneq ($(findstring debug,$(CONF)),)
    compile_flags += -ggdb
endif
ifneq ($(findstring release,$(CONF)),)
    # Including -Ofast when linking (by including $(optimizations)) gives a
    # different binary size. Might be worth investigating why.
    compile_flags += $(optimizations) -DOPTIMIZING
    link_flags    += $(optimizations) -fuse-linker-plugin
endif

ifeq ($(BACKTRACE_SUPPORT),1)
    # No -rdynamic support in older Clang versions. This is equivalent.
    link_flags += -Wl,-export-dynamic
endif

ifeq ($(INCLUDE_DEBUGGER),1)
    compile_flags += -DINCLUDE_DEBUGGER
endif

ifeq ($(RECORD_MOVIE),1)
    compile_flags += -DRECORD_MOVIE
endif

ifeq ($(TEST),1)
    compile_flags += -DRUN_TESTS
endif

# _FILE_OFFSET_BITS=64 gives nicer errors for large files (even though we don't
# support them on 32-bit systems)
compile_flags += $(warnings) -D_FILE_OFFSET_BITS=64 $(shell sdl2-config --cflags)

#
# Targets
#

$(BUILD_DIR)/$(EXECUTABLE): $(objects)
	@echo Linking $@
	$(q)$(CXX) $(link_flags) $(EXTRA_LINK) $^ $(LDLIBS) -o $@

$(cpp_objects): $(BUILD_DIR)/%.o: src/%.cpp
	@echo Compiling $<
	$(q)$(CXX) -c -Iinclude $(compile_flags) $(EXTRA) $< -o $@

$(c_objects): $(BUILD_DIR)/%.o: src/%.c
	@echo Compiling $<
	$(q)$(CC) -c -Iinclude -std=c11 $(compile_flags) $(EXTRA) $< -o $@

# Automatic generation of prerequisites:
# http://www.gnu.org/software/make/manual/make.html#Automatic-Prerequisites
# Modified to use a separate build directory and a list of sources (via a
# static pattern rule) rather than a catch-all wildcard.
$(deps): $(BUILD_DIR)/%.d: src/%.cpp
	@set -e; rm -f $@;                                                 \
	  $(CXX) -MM -Iinclude $(shell sdl2-config --cflags) $< > $@.$$$$; \
	  sed 's,\($*\)\.o[ :]*,$(BUILD_DIR)/\1.o $@ : ,g' < $@.$$$$ > $@; \
	  rm -f $@.$$$$

ifneq ($(MAKECMDGOALS),clean)
    # The .d files that hold the automatically generated dependencies. One per
    # source file.
    -include $(deps)
endif

$(BUILD_DIR): ; $(q)mkdir $(BUILD_DIR)
# The objects and automatic prerequisite files need the build directory to
# exist, but shouldn't be affected by modifications to its contents. Hence an
# order-only dependency.
$(objects) $(deps): | $(BUILD_DIR)

.PHONY: clean
clean: ; $(q)-rm -rf $(BUILD_DIR)
