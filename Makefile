#
# Configuration variables
#

CXX               := g++
NAME              := nesalizer
# Separate build directory
OBJDIR            := build
# Extra flags passed during compilation and linking
EXTRA             :=
EXTRA_LINK        :=
# "debug", "release", or "release-debug". "release-debug" adds debugging
# information in addition to optimizing.
CONF              := debug
# If "1", passes -rdynamic to add symbols for backtraces
BACKTRACE_SUPPORT := 1
# If "1", configures for automatic test ROM running
TEST              := 0

# If V is "1", commands are printed as they are executed
ifneq ($(V),1)
    q := @
endif

# 1 if "clang" occurs in CXX, otherwise 0
is_clang := $(if $(findstring clang,$(CXX)),1,0)

#
# Source files and libraries
#

objects :=                                      \
  audio.o apu.o blip_buf.o controller.o cpu.o   \
  debug.o error.o input.o main.o md5.o mapper.o \
  mapper_0.o mapper_1.o mapper_2.o mapper_3.o   \
  mapper_4.o mapper_5.o mapper_7.o mapper_9.o   \
  mapper_11.o mapper_71.o mapper_232.o ppu.o    \
  rom.o save_states.o sdl_backend.o timing.o    \
  test.o util.o
sources        := $(objects:.o=.cpp)
objdir_objects := $(addprefix $(OBJDIR)/,$(objects))
objdir_deps    := $(addprefix $(OBJDIR)/,$(sources:.cpp=.d))

LDLIBS := -lreadline $(shell sdl2-config --libs) -lrt

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

optimizations += -msse3 -flto -fno-exceptions -fno-stack-protector -fno-rtti -DNDEBUG
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

ifeq ($(TEST),1)
    compile_flags += -DRUN_TESTS
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

$(OBJDIR)/$(NAME): $(objdir_objects)
	@echo Linking $@
	$(q)$(CXX) $(link_flags) $(EXTRA_LINK) $^ $(LDLIBS) -o $@

$(OBJDIR)/%.o : %.cpp
	@echo Compiling $<
	$(q)$(CXX) -c $(compile_flags) $(EXTRA) $< -o $@

# Automatic generation of prerequisites:
# http://www.gnu.org/software/make/manual/make.html#Automatic-Prerequisites
# Modified to use a separate build directory and a list of sources (via a
# static pattern rule) rather than a catch-all wildcard.
$(objdir_deps): $(OBJDIR)/%.d: %.cpp
	@set -e; rm -f $@;                                              \
	  $(CXX) -MM $(shell sdl2-config --cflags) $< > $@.$$$$;        \
	  sed 's,\($*\)\.o[ :]*,$(OBJDIR)/\1.o $@ : ,g' < $@.$$$$ > $@; \
	  rm -f $@.$$$$

ifneq ($(MAKECMDGOALS),clean)
    # The .d files that hold the automatically generated dependencies. One per
    # source file.
    -include $(objdir_deps)
endif

$(OBJDIR): ; $(q)mkdir $(OBJDIR)
# The objects and automatic prerequisite files need the build directory to
# exist, but shouldn't be affected by modifications to its contents. Hence an
# order-only dependency.
$(objdir_objects) $(objdir_deps): | $(OBJDIR)

.PHONY: clean
clean: ; $(q)-rm -rf $(OBJDIR)
