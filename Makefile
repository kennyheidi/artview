#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

# Fall back to the standard install location (devkitPro MSYS2 on Windows, or Linux/macOS)
# so the build works even from a plain MSYS2 shell without the env vars set.
ifeq ($(strip $(DEVKITPRO)),)
export DEVKITPRO := /opt/devkitpro
endif
ifeq ($(strip $(DEVKITARM)),)
export DEVKITARM := $(DEVKITPRO)/devkitARM
endif
ifeq ($(wildcard $(DEVKITARM)/3ds_rules),)
$(error "devkitARM not found at $(DEVKITARM). Install devkitPro (3ds-dev) or set DEVKITPRO/DEVKITARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Which site(s) to build for:
#   make                  -> artview: rule34.xxx + Derpibooru + DeviantArt in one app (site tab)
#   make SITE=rule34      -> r34view    (rule34.xxx only)
#   make SITE=derpibooru  -> derpiview  (Derpibooru only)
#   make SITE=deviantart  -> daview     (DeviantArt only)
#   make SITE=e621        -> e6view     (e621 only)
#   make SITE=pixiv       -> pxview     (pixiv only)
# Separate object folders, outputs, title IDs and art. All of them keep each
# site's data in sdmc:/3ds/r34view, /derpiview, /daview, /e6view and /pxview, so they share galleries.
SITE ?= all

ifeq ($(SITE),derpibooru)
TARGET      := derpiview
BUILD       := build-derpi
META        := meta-derpi
SITE_DEFS   := -DSITE_DERPIBOORU
APP_TITLE        := derpiview
APP_DESCRIPTION  := Derpibooru viewer
APP_PRODUCT_CODE := CTR-H-DRPV
APP_UNIQUE_ID    := 0xB3E5
else ifeq ($(SITE),deviantart)
TARGET      := daview
BUILD       := build-da
META        := meta-da
SITE_DEFS   := -DSITE_DEVIANTART
APP_TITLE        := daview
APP_DESCRIPTION  := DeviantArt viewer
APP_PRODUCT_CODE := CTR-H-DAVW
APP_UNIQUE_ID    := 0xB3E6
else ifeq ($(SITE),e621)
TARGET      := e6view
BUILD       := build-e6
META        := meta-e6
SITE_DEFS   := -DSITE_E621
APP_TITLE        := e6view
APP_DESCRIPTION  := e621 viewer
APP_PRODUCT_CODE := CTR-H-E6VW
APP_UNIQUE_ID    := 0xB3E8
else ifeq ($(SITE),pixiv)
TARGET      := pxview
BUILD       := build-px
META        := meta-px
SITE_DEFS   := -DSITE_PIXIV
APP_TITLE        := pxview
APP_DESCRIPTION  := pixiv viewer
APP_PRODUCT_CODE := CTR-H-PXVW
APP_UNIQUE_ID    := 0xB3E9
else ifeq ($(SITE),rule34)
TARGET      := r34view
BUILD       := build
META        := meta
SITE_DEFS   :=
APP_TITLE        := r34view
APP_DESCRIPTION  := rule34.xxx viewer
APP_PRODUCT_CODE := CTR-H-R34V
APP_UNIQUE_ID    := 0xB3E4
else ifeq ($(SITE),all)
TARGET      := artview
BUILD       := build-all
META        := meta-all
SITE_DEFS   := -DSITE_MULTI
APP_TITLE        := artview
APP_DESCRIPTION  := rule34 / Derpibooru / DeviantArt / e621 / pixiv viewer
APP_PRODUCT_CODE := CTR-H-ARTV
APP_UNIQUE_ID    := 0xB3E7
else
$(error SITE must be all, rule34, derpibooru, deviantart, e621 or pixiv)
endif

SOURCES     := source source/quirc source/helix-aac source/qrgen
DATA        := data
INCLUDES    := include
GRAPHICS    :=
ROMFS       := romfs

APP_AUTHOR      := homebrew
ICON            := $(META)/icon.png

# .cia settings (make cia). Title ID = 00040000 00 + unique id + 00
# (artview 0004000000B3E700, r34view ...B3E400, derpiview ...B3E500, daview ...B3E600, e6view ...B3E800, pxview ...B3E900).
BANNER_IMAGE     := $(META)/banner.png
BANNER_AUDIO     := $(META)/audio.wav
RSF              := meta/app.rsf

# makerom / bannertool: looked up in tools/ first, then PATH
MAKEROM    ?= $(firstword $(wildcard $(CURDIR)/tools/makerom.exe $(CURDIR)/tools/makerom) makerom)
BANNERTOOL ?= $(firstword $(wildcard $(CURDIR)/tools/bannertool.exe $(CURDIR)/tools/bannertool) bannertool)

#---------------------------------------------------------------------------------
ARCH     := -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS   := -g -Wall -O2 -mword-relocations -ffunction-sections \
            $(ARCH) $(INCLUDE) -D__3DS__ $(SITE_DEFS)
ASFLAGS  := -g $(ARCH)
LDFLAGS   = -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS     := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lcitro2d -lcitro3d -lctru -lm
LIBDIRS  := $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                   $(foreach dir,$(DATA),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES   := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD       := $(CC)
export OFILES   := $(CFILES:.c=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        export APP_ICON := $(CTRULIB)/default_icon.png
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

.PHONY: all clean cia check-cia-tools FORCE

all: $(ROMFS)/cacert.pem
	@mkdir -p $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

check-cia-tools:
	@command -v "$(MAKEROM)" >/dev/null 2>&1 || { echo "makerom not found. Put makerom(.exe) in tools/ or on PATH:"; \
	  echo "  https://github.com/3DSGuy/Project_CTR/releases"; exit 1; }
	@command -v "$(BANNERTOOL)" >/dev/null 2>&1 || { echo "bannertool not found. Put bannertool(.exe) in tools/ or on PATH:"; \
	  echo "  https://github.com/carstene1ns/3ds-bannertool/releases"; exit 1; }

# Always rebuilt (FORCE): unzipping keeps old file dates, so a newer-looking stale
# banner would otherwise be kept and the old sound/image would end up in the CIA.
$(BUILD)/banner.bnr: $(BANNER_IMAGE) $(BANNER_AUDIO) FORCE
	@mkdir -p $(BUILD)
	@"$(BANNERTOOL)" makebanner -i $(BANNER_IMAGE) -a $(BANNER_AUDIO) -o $@

$(BUILD)/icon.icn: $(ICON) FORCE
	@mkdir -p $(BUILD)
	@"$(BANNERTOOL)" makesmdh -s "$(APP_TITLE)" -l "$(APP_DESCRIPTION)" -p "$(APP_AUTHOR)" -i $(ICON) -o $@

cia: check-cia-tools all $(BUILD)/banner.bnr $(BUILD)/icon.icn
	@"$(MAKEROM)" -f cia -o $(TARGET).cia -target t -exefslogo \
	  -elf $(TARGET).elf -rsf $(RSF) -icon $(BUILD)/icon.icn -banner $(BUILD)/banner.bnr \
	  -DAPP_TITLE="$(APP_TITLE)" -DAPP_PRODUCT_CODE="$(APP_PRODUCT_CODE)" \
	  -DAPP_UNIQUE_ID="$(APP_UNIQUE_ID)" -DAPP_ROMFS="$(ROMFS)"
	@echo built ... $(TARGET).cia

# Mozilla CA bundle (shipped in romfs/). Only re-fetched if someone deleted it.
$(ROMFS)/cacert.pem:
	@mkdir -p $(ROMFS)
	curl -fsSL -o $@ https://curl.se/ca/cacert.pem || wget -qO $@ https://curl.se/ca/cacert.pem

FORCE:

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------

$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh
$(OUTPUT).elf: $(OFILES)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
