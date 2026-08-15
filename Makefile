#---------------------------------------------------------------------------------
# A-Theme Installer — Switch homebrew NRO
#
# Based on devkitPro's standard switch-example NRO Makefile template.
# If `make` fails on your setup, grab a fresh template from
# https://github.com/devkitPro/switch-examples and diff against this one —
# devkitPro updates their base templates occasionally and this file may
# drift out of date with a very new toolchain release.
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to devkitpro>")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# App metadata — shown on the Switch home menu / hbmenu
#---------------------------------------------------------------------------------
APP_TITLE   := A-Theme Installer
APP_AUTHOR  := A-Theme
APP_VERSION := 2.0.2
# Switch homebrew icons must be 256x256 JPEG -- icon.jpg in this folder
# is exactly that (the A-Theme logo, flattened onto its own dark
# background since JPEG has no alpha channel).
APP_ICON    := icon.jpg

#---------------------------------------------------------------------------------
# Build config
#---------------------------------------------------------------------------------
TARGET      := a-theme-installer
BUILD       := build
SOURCES     := source
DATA        := data
INCLUDES    := include
ROMFS       := romfs

ARCH    := -march=armv8-a -mtune=cortex-a57 -mtp=soft -fPIE

# SDL2 + SDL2_image + SDL2_ttf render the whole app now (menu and preview
# both) — following devkitPro's own official example pattern here
# (switch-examples' graphics/sdl2/sdl2-simple/Makefile) rather than
# hand-listing the full transitive link chain (EGL, drm_nouveau, libpng,
# libjpeg-turbo, freetype, etc.) — pkg-config resolves whatever your
# installed portlib versions actually need.
PKGCONF := aarch64-none-elf-pkg-config
PC_LIBS := sdl2 SDL2_image SDL2_ttf

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__
CFLAGS  += `$(PKGCONF) --cflags $(PC_LIBS)`
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl needs mbedtls (TLS) and zlib (compression) underneath it on Switch.
# zziplib reads the downloaded .zip theme archives, and itself needs zlib too.
LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lzzip -lz \
           `$(PKGCONF) --libs $(PC_LIBS)` -lnx

#---------------------------------------------------------------------------------
# Library / portlib search paths
#---------------------------------------------------------------------------------
LIBDIRS := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)

export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES    := $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

export LD   := $(CC)

export OFILES_BIN  := $(addsuffix .o,$(BINFILES))
export OFILES_SRC   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES   := $(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN   := $(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ROMFS)),)
export NROFLAGS :=
else
export NROFLAGS := --romfsdir=$(CURDIR)/$(ROMFS)
endif

ifeq ($(strip $(APP_TITLE)),)
else
	NACPFLAGS += --name="$(APP_TITLE)"
endif
ifeq ($(strip $(APP_AUTHOR)),)
else
	NACPFLAGS += --author="$(APP_AUTHOR)"
endif
ifeq ($(strip $(APP_VERSION)),)
else
	NACPFLAGS += --version="$(APP_VERSION)"
endif
ifeq ($(strip $(APP_ICON)),)
else
	# Anchored to $(TOPDIR), not left relative: the build re-invokes make
	# from inside build/, so a bare "icon.jpg" would be looked up in
	# build/icon.jpg and fail with "Failed to open input icon!". Same
	# reason --romfsdir above uses an absolute path.
	NROFLAGS += --icon=$(TOPDIR)/$(APP_ICON)
endif

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
