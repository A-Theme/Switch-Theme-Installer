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
APP_VERSION := 1.0.0
# ICON left unset on purpose — hbmenu will use its default icon.
# Set APP_ICON := icon.jpg (256x256) here if you want a custom one.

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

CFLAGS  := -g -Wall -O2 -ffunction-sections $(ARCH) $(DEFINES)
CFLAGS  += $(INCLUDE) -D__SWITCH__
CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++20

ASFLAGS := -g $(ARCH)
LDFLAGS  = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

# curl needs mbedtls (TLS) and zlib (compression) underneath it on Switch.
LIBS    := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lnx

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
	NROFLAGS += --icon=$(APP_ICON)
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
