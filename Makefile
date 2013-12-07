#
# Configuration variables
#

CXX               := g++
CC                := gcc
NAME              := nesalizer
# Separate build directory
OBJDIR            := build
# Extra flags passed during compilation and linking
EXTRA             :=
EXTRA_LINK        :=
# "debug", "release", or "release-debug". "release-debug" adds debugging
# information in addition to optimizing.
CONF              := debug
# If "1", includes a simple debugger (see cpu.cpp) for internal use. Has
# readline dependency.
INCLUDE_DEBUGGER  := 0
# If "1", a movie is recorded to movie.mp4 using libav (movie.cpp)
RECORD_MOVIE      := 0
# If "1", passes -rdynamic to add symbols for backtraces
BACKTRACE_SUPPORT := 1
# If "1", configures for automatic test ROM running
TEST              := 0

# If V is "1", commands are printed as they are executed
ifneq ($(V),1)
    q := @
endif

# 1 if "clang" occurs in the output of CXX -v, otherwise 0
is_clang := $(if $(findstring clang,$(shell "$(CXX)" -v 2>&1)),1,0)

#
# Source files and libraries
#

cpp_sources :=                                  \
  audio apu blip_buf controller cpu debug error \
  input main md5 mapper mapper_0 mapper_1       \
  mapper_2 mapper_3 mapper_4 mapper_5 mapper_7  \
  mapper_9 mapper_11 mapper_71 mapper_232 ppu   \
  rom save_states sdl_backend timing util
# Use C99 for the handy designated initializers feature
c_sources   := tables

ifeq ($(RECORD_MOVIE),1)
    cpp_sources += movie
endif
ifeq ($(TEST),1)
    cpp_sources += test
endif

cpp_objects := $(addprefix $(OBJDIR)/,$(cpp_sources:=.o))
c_objects   := $(addprefix $(OBJDIR)/,$(c_sources:=.o))
objects     := $(c_objects) $(cpp_objects)
deps        := $(addprefix $(OBJDIR)/,$(c_sources:=.d) $(cpp_sources:=.d))

LDLIBS := $(shell sdl2-config --libs) -lrt
ifeq ($(INCLUDE_DEBUGGER),1)
    LDLIBS += -lreadline
endif
ifeq ($(RECORD_MOVIE),1)
    LDLIBS += -lavcodec -lavformat -lavutil -lswscale
endif

#
# Debugging and optimization
#

# Dubious optimizations:
#   -fno-stack-protector
#   -U_FORTIFY_SOURCE
#   -funroll-loops

ifeq ($(is_clang),1)
    optimizations := -O3 -ffast-math
else
    # Assume GCC
    optimizations := -Ofast -mfpmath=sse -funsafe-loop-optimizations
endif

optimizations += -msse3 -flto -fno-exceptions -fno-stack-protector -DNDEBUG
# Flags passed in addition to the above during linking
link_optimizations := -fuse-linker-plugin

warnings :=                                        \
  -Wall -Wextra -Wdisabled-optimization            \
  -Wmissing-format-attribute -Wmaybe-uninitialized \
  -Wno-switch -Wuninitialized                      \
  -Wunsafe-loop-optimizations

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

ifneq ($(filter debug release-debug,$(CONF)),)
    add_debug_info := 1
endif
ifneq ($(filter release release-debug,$(CONF)),)
    optimize := 1
endif

# The GCC docs aren't too clear on which flags are needed during linking. Add
# them all to be safe.
ifeq ($(add_debug_info),1)
    compile_flags += -ggdb
    link_flags    += -ggdb
endif

ifeq ($(optimize),1)
    compile_flags += $(optimizations) -DOPTIMIZING
    link_flags    += $(optimizations) $(link_optimizations)
endif

ifeq ($(BACKTRACE_SUPPORT),1)
    # No -rdynamic support in Clang
    ifeq ($(is_clang),0)
        compile_flags += -rdynamic
        link_flags    += -rdynamic
    endif
endif

ifeq ($(RECORD_MOVIE),1)
    compile_flags += -DRECORD_MOVIE
endif

ifeq ($(TEST),1)
    compile_flags += -DRUN_TESTS
endif

ifeq ($(INCLUDE_DEBUGGER),1)
    compile_flags += -DINCLUDE_DEBUGGER
endif

# Gives nicer errors for large files (even though we don't support them on
# 32-bit systems)
compile_flags += -D_FILE_OFFSET_BITS=64
# SDL2 stuff
compile_flags += $(shell sdl2-config --cflags)

# Save states may involve unsafe type punning
compile_flags += -fno-strict-aliasing

#
# Targets
#

$(OBJDIR)/$(NAME): $(objects)
	@echo Linking $@
	$(q)$(CXX) $(link_flags) $(EXTRA_LINK) $^ $(LDLIBS) -o $@

$(cpp_objects): $(OBJDIR)/%.o: %.cpp
	@echo Compiling $<
	$(q)$(CXX) -c $(compile_flags) $(EXTRA) $< -o $@

$(c_objects): $(OBJDIR)/%.o: %.c
	@echo Compiling $<
	$(q)$(CC) -c -std=c99 $(compile_flags) $(EXTRA) $< -o $@

# Automatic generation of prerequisites:
# http://www.gnu.org/software/make/manual/make.html#Automatic-Prerequisites
# Modified to use a separate build directory and a list of sources (via a
# static pattern rule) rather than a catch-all wildcard.
$(deps): $(OBJDIR)/%.d: %.cpp
	@set -e; rm -f $@;                                              \
	  $(CXX) -MM $(shell sdl2-config --cflags) $< > $@.$$$$;        \
	  sed 's,\($*\)\.o[ :]*,$(OBJDIR)/\1.o $@ : ,g' < $@.$$$$ > $@; \
	  rm -f $@.$$$$

ifneq ($(MAKECMDGOALS),clean)
    # The .d files that hold the automatically generated dependencies. One per
    # source file.
    -include $(deps)
endif

$(OBJDIR): ; $(q)mkdir $(OBJDIR)
# The objects and automatic prerequisite files need the build directory to
# exist, but shouldn't be affected by modifications to its contents. Hence an
# order-only dependency.
$(objects) $(deps): | $(OBJDIR)

.PHONY: clean
clean: ; $(q)-rm -rf $(OBJDIR)
