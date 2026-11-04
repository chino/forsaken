#
# General Compiler Settings
#

# for cross compiling give the toolchain prefix (see README.md) ex:
# make CROSS=aarch64-linux-gnu-
# or the compiler ex: make CC=i686-mingw32-gcc
CROSS=
CC=$(CROSS)gcc

# $(CROSS)pkg-config when it exists
PKG_CONFIG?=$(if $(CROSS),$(shell command -v $(CROSS)pkg-config >/dev/null 2>&1 && \
              echo $(CROSS)pkg-config || echo pkg-config),pkg-config)

# board presets
# OpenPandora: Cortex-A8, SDL 2 (pandora video driver), OpenGL ES 1.1
ifeq ($(PANDORA),1)
  SDL?=2
  GLES?=1
  PROFILE?=0
  FLAGS+= -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=softfp
  GLES_LIBS?= -lGLES_CM
  PLATFORM_CFLAGS+= -DPANDORA
endif

# general compiler settings
ifeq ($(M32),1)
  FLAGS+= -m32
endif
# the code assumes char is signed like on x86, it isn't on ARM or PowerPC
FLAGS+= -std=gnu99 -pipe -fcommon -fsigned-char
CFLAGS=$(FLAGS) -Wall -Wextra
LDFLAGS=$(FLAGS)

# right now non debug build would probably crash anyway
# we even release debug builds as the official release
DEBUG=1

# might as well leave gprof support on by default as well
PROFILE?=1

# use this if you want to build everything statically
STATIC=0

# Mudflap is a pointer use checking library. For more info:
# http://gcc.gnu.org/wiki/Mudflap_Pointer_Debugging
MUDFLAP=0

ifeq ($(MUDFLAP),1)
  ifeq ($(DEBUG),1)
    FLAGS+= -fmudflap
    LIB+= -lmudflap
  else
    X:=$(error Mudflap enabled without debug mode - probably not what you meant)
  endif
endif

# stack-smash protection against buffer overflows and corrupt pointers
# (enabled by default on many systems today)
ifeq ($(SSP),1)
  FLAGS+= -fstack-protector-all
  CFLAGS+= -D_FORTIFY_SOURCE=2
endif

ifeq ($(DEBUG),1)
  FLAGS+= -g
else
  CFLAGS+=-O3 -Winit-self
  LDFLAGS+=-s
endif

ifeq ($(PROFILE),1)
  ifneq ($(DEBUG),1)
    # Debug symbols needed for profiling to be useful
    FLAGS+= -g
  endif
  FLAGS+= -pg
endif

#
# ProjectX Specific
#

MACOSX=$(shell uname -a | grep -qi darwin && echo 1 || echo 0)

# lua scripting (config loader, public game list, bots)
LUA=0
ifeq ($(LUA),1)
  # some systems use lua5.1
  LUA_PKG=$(shell $(PKG_CONFIG) lua && echo lua || echo lua5.1)
  # debian/ubuntu split luasocket's mime module into its own package
  LUA_MIME=$(shell $(PKG_CONFIG) --exists $(LUA_PKG)-mime && echo $(LUA_PKG)-mime)
  LUA_LIBS=$(LUA_PKG) $(LUA_PKG)-socket $(LUA_MIME)
endif

# which version of sdl do you want to ask pkgconfig for ?
SDL?=1
ifeq ($(SDL),1)
  SDL_=sdl
else
  SDL_=sdl$(SDL)
endif

# which version of GL do you want to use ?
GL?=1

$(if $(shell test "$(GL)" -ge 3 -a "$(SDL)" -lt 2 && echo fail), \
     $(error "GL >= 3 only supported with SDL >= 2"))

# use OpenGL ES 1.1 instead of desktop OpenGL (ARM boards, phones...)
# SDL 2 creates the context, SDL 1.2 can not.
GLES?=0
ifeq ($(GLES),1)
  ifneq ($(GL),1)
    $(error "GLES=1 is only supported with GL=1")
  endif
  ifneq ($(SDL),2)
    $(error "GLES=1 is only supported with SDL=2")
  endif
  GLES_CFLAGS?=$(shell $(PKG_CONFIG) --cflags glesv1_cm 2>/dev/null)
  GLES_LIBS?=$(shell $(PKG_CONFIG) --libs glesv1_cm 2>/dev/null || echo -lGLESv1_CM)
  CFLAGS+= -DGLES $(GLES_CFLAGS)
endif

# library headers
CFLAGS+= `$(PKG_CONFIG) --cflags $(SDL_) $(LUA_LIBS) libenet libpng zlib openal`
CFLAGS+= $(PLATFORM_CFLAGS)
ifeq ($(MACOSX),1)
  CFLAGS += -DMACOSX
endif

# libraries
ifeq ($(STATIC),1)
  ifeq ($(MACOSX),1)
     $(error MacOSX does not support static builds)
  endif
  LIB+= -Wl,-dn
  PKG_CFG_OPTS= --static
endif
LIB+= `$(PKG_CONFIG) $(PKG_CFG_OPTS) --libs $(LUA_LIBS) libenet libpng zlib openal` -lm
ifeq ($(STATIC),1)
  LIB+= -Wl,-dy
endif

# dynamic only libraries
LIB+= `$(PKG_CONFIG) --libs $(SDL_)`
ifeq ($(MINGW),1)
  CFLAGS += -DMINGW
  LIB += -L./mingw/bin
  LIB += -lglu32 -lopengl32
  LIB += -lsocket -lws2_32 -lwsock32 -lwinmm -lOpenAL32
else ifeq ($(MACOSX),1)
  # TODO - Support targeting X11 (need to compile sdl properly)
  #LIB += -L/usr/X11/lib/ -lGL -lGLU
  LIB += -framework OpenGL # OpenGL bundle on OSX.
  LIB += -framework Cocoa  # Used to target Quartz by SDL_.
else ifeq ($(GLES),1)
  LIB += $(GLES_LIBS)
else
  LIB += -lGL -lGLU
endif
ifneq ($(MINGW),1)
  # apparently on some systems -ldl is explicitly required
  # perhaps this is part of the default libs on others...?
  LIB+= -ldl
endif

# ProjectX-specific includes
CFLAGS += -I.

# ProjectX-specific macros
ifeq ($(DXMOUSE),1)
  CFLAGS += -DDXMOUSE -Idinput
  LIB += -Ldinput -ldinput -ldxguid
endif
ifeq ($(RENDER_DISABLED),1)
  CFLAGS+= -DRENDER_DISABLED
else
  CFLAGS+= -DGL=$(GL)
endif
CFLAGS+= -DNET_ENET_2 -DBSP -DTEXTURE_PNG -DSOUND_SUPPORT -DSOUND_OPENAL
ifeq ($(DEBUG),1)
  CFLAGS+= -DDEBUG_ON -DDEBUG_COMP -DDEBUG_SPOTFX_SOUND -DDEBUG_VIEWPORT
endif

ifeq ($(LUA),1)
  CFLAGS+= -DLUA -DLUA_USE_APICHECK
endif

ifeq ($(BOT),1)
  ifneq ($(LUA),1)
    X:=$(error BOT=1 requires LUA=1)
  endif
  CFLAGS+= -DLUA_BOT
endif

ifeq ($(INPUT_DISABLED),1)
  CFLAGS+= -DINPUT_DISABLED
endif

# Streamed soundtrack (music.c), replacing the original CD audio.
# MUSIC=0 builds without it. WAV support is built in; Ogg Vorbis and FLAC
# are enabled when pkg-config finds vorbisfile / flac, or force them with
# MUSIC_OGG=0|1 and MUSIC_FLAC=0|1 (without pkg-config, e.g. when cross
# compiling, plain -l flags are used; a static libFLAC also wants
# ADD_CFLAGS=-DFLAC__NO_DLL).
MUSIC=1
ifeq ($(MUSIC),1)
  MUSIC_OGG:=$(shell $(PKG_CONFIG) --exists vorbisfile 2>/dev/null && echo 1 || echo 0)
  MUSIC_FLAC:=$(shell $(PKG_CONFIG) --exists flac 2>/dev/null && echo 1 || echo 0)
  CFLAGS+= -DMUSIC
  ifeq ($(MUSIC_OGG),1)
    CFLAGS+= -DMUSIC_OGG $(shell $(PKG_CONFIG) --cflags vorbisfile 2>/dev/null)
    LIB+= $(shell $(PKG_CONFIG) $(PKG_CFG_OPTS) --libs vorbisfile 2>/dev/null || echo -lvorbisfile -lvorbis -logg)
  endif
  ifeq ($(MUSIC_FLAC),1)
    CFLAGS+= -DMUSIC_FLAC $(shell $(PKG_CONFIG) --cflags flac 2>/dev/null)
    LIB+= $(shell $(PKG_CONFIG) $(PKG_CFG_OPTS) --libs flac 2>/dev/null || echo -lFLAC)
  endif
else
  MUSIC_OGG=0
  MUSIC_FLAC=0
endif

# OpenXR head mounted display support, built in when the OpenXR headers
# (libopenxr-dev) are found; VR=0 leaves it out. The loader is opened at
# run time and only with -vr, so the binary does not depend on it; without
# a loader, runtime or headset the game stays on the desktop. See docs/VR.md.
ifeq ($(MACOSX)$(RENDER_DISABLED)$(GLES),00)
  VR_PKGS=openxr $(if $(filter 1,$(MINGW)),,x11)
  VR:=$(shell $(PKG_CONFIG) --exists $(VR_PKGS) && echo 1 || echo 0)
endif
ifeq ($(VR),1)
  ifeq ($(RENDER_DISABLED),1)
    $(error "VR=1 needs a GL renderer")
  endif
  ifeq ($(GLES),1)
    $(error "VR=1 needs desktop OpenGL, not GLES")
  endif
  CFLAGS += -DVR_OPENXR `$(PKG_CONFIG) --cflags openxr`
  ifneq ($(MINGW),1)
    ifneq ($(MACOSX),1)
      LIB += `$(PKG_CONFIG) --libs x11`
    endif
  endif
endif


INC=$(wildcard *.h)
SRC=$(wildcard *.c)
ifneq ($(LUA),1)
  SRC:=$(filter-out lua_%.c,$(SRC))
endif
ifneq ($(VR),1)
  SRC:=$(filter-out vr_openxr.c,$(SRC))
endif
OBJ=$(patsubst %.c,%.o,$(SRC))

# allows user to override settings
ADD_FLAGS=
ADD_CFLAGS=
ADD_LDFLAGS=
FLAGS+=$(ADD_FLAGS)
CFLAGS+=$(ADD_CFLAGS)
LDFLAGS+=$(ADD_LDFLAGS)

BIN=projectx

all: $(BIN)

$(BIN): $(OBJ)
	$(CC) -o $(BIN) $(OBJ) $(LDFLAGS) $(LIB)

$(OBJ): $(INC)

clean:
	$(RM) *.o $(BIN)

check:
	@echo
	@echo "INC = $(INC)"
	@echo
	@echo "SRC = $(SRC)"
	@echo
	@echo "OBJ = $(OBJ)"
	@echo
	@echo "DEBUG = $(DEBUG)"
	@echo "PROFILE = $(PROFILE)"
	@echo "MUDFLAP = $(MUDFLAP)"
	@echo "STATIC = $(STATIC)"
	@echo "PKG_CFG_OPTS = $(PKG_CFG_OPTS)"
	@echo "MINGW = $(MINGW)"
	@echo "CROSS = $(CROSS)"
	@echo "BOT = $(BOT)"
	@echo "GL = $(GL)"
	@echo "GLES = $(GLES)"
	@echo "PANDORA = $(PANDORA)"
	@echo "PKG_CONFIG = $(PKG_CONFIG)"
	@echo "RENDER_DISABLED = $(RENDER_DISABLED)"
	@echo "INPUT_DISABLED = $(INPUT_DISABLED)"
	@echo "MUSIC = $(MUSIC) (ogg $(MUSIC_OGG), flac $(MUSIC_FLAC))"
	@echo "LUA = $(LUA)"
	@echo "LUA_PKG = $(LUA_PKG)"
	@echo "SDL = $(SDL)"
	@echo "SDL_ = $(SDL_)"
	@echo "VR = $(VR)"
	@echo
	@echo "CC = $(CC)"
	@echo "BIN = $(BIN)"
	@echo "CFLAGS = $(CFLAGS)"
	@echo "LDFLAGS = $(LDFLAGS)"
	@echo "LIB = $(LIB)"
	@echo

.PHONY: all clean
