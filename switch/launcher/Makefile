# ARMSX2-nx launcher for the Switch 2 (kaiHEN): armsx2-nx2.nro, an SDL 2 game library that writes the cores' settings
# (inis/PCSX2.ini) and chainloads the renderer's core. Built with libounce and the Switch 2 Mesa SDK's EGL.
#
#   make [CORE_DRAKO3D=<armsx2_drako3d.nro>] [CORE_VULKAN=<armsx2_vulkan.nro>] [SWITCH2_MESA_SDK=../deps/switch2_mesa_sdk]
#
# The cores (switch/build-switch2.sh drako3d and vulkan) travel in the NRO's romfs (emu/), with their SHA-256 for the
# launcher's copy on the SD card: switch/build-switch2-all.sh builds both, then this.
.SUFFIXES:
.DEFAULT_GOAL := all

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif
ifeq ($(wildcard $(DEVKITPRO)/libounce/switch2_rules),)
$(error "No Switch 2 devkit (libounce) in $(DEVKITPRO): install https://github.com/ouncebrew/ounce-devkit")
endif

APP_TITLE	:=	ARMSX2-nx
APP_AUTHOR	:=	ARMSX2 Team, PalindromicBreadLoaf & NaGaa95
RELEASE_VERSION	?=	1.0.0
APP_VERSION	?=	$(RELEASE_VERSION)

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libounce/switch2_rules

TARGET		:=	armsx2-nx2
BUILD		:=	build
SOURCES		:=	source ../nx2
DATA		:=	data
INCLUDES	:=	source ../nx2
# romfs/ and the cores, staged
ROMFS		:=	$(BUILD)/romfs
ICON		:=	icon.jpg
SWITCH2_MESA_SDK	?=	$(TOPDIR)/../deps/switch2_mesa_sdk
CORE_DRAKO3D	?=	$(TOPDIR)/../../build-switch2-drako3d/switch/app/armsx2_drako3d.nro
CORE_VULKAN	?=	$(TOPDIR)/../../build-switch2-vulkan/switch/app/armsx2_vulkan.nro

ARCH	:=	$(SWITCH2_CPU) -mtp=soft -fPIE
LTOFLAGS	:=	-flto=auto -fuse-linker-plugin

# SDL 2 and curl come from the Switch portlibs; their pkg-config files name switch-mesa's EGL, so their flags are given
# here. EGL comes from the Switch 2 Mesa SDK.
SDL_CFLAGS	:=	-I$(PORTLIBS)/include/SDL2 -D_REENTRANT -DCURL_STATICLIB

CFLAGS	:=	$(SDL_CFLAGS) -Wall -Wextra -O3 -DNDEBUG $(LTOFLAGS) -ffunction-sections -fdata-sections -fno-ident \
			-ffile-prefix-map=$(CURDIR)=. -fmacro-prefix-map=$(CURDIR)=. \
			-DARMSX2NX_VERSION='"$(APP_VERSION)"' \
			-DARMSX2NX_RELEASE_VERSION='"$(RELEASE_VERSION)"' $(ARCH) $(DEFINES)
CFLAGS	+=	$(INCLUDE) -D_GNU_SOURCE -D_LARGEFILE64_SOURCE -D_FILE_OFFSET_BITS=64

CXXFLAGS	:= $(CFLAGS) -fno-rtti -std=gnu++20

ASFLAGS	:=	$(ARCH)
# nx2_platform.c and the link wraps it needs (see nx2_platform.h).
LDFLAGS	=	-specs=$(DEVKITPRO)/libounce/switch.specs $(ARCH) $(LTOFLAGS) -Wl,-Map,$(notdir $*.map) \
			-Wl,--gc-sections -Wl,--build-id=none \
			-Wl,--wrap=hidInitializeKeyboard -Wl,--wrap=hidInitializeMouse \
			-Wl,--wrap=hidInitializeTouchScreen -Wl,--wrap=__syscall_thread_create \
			-Wl,--wrap=smGetServiceWrapper

# (the SDK's libEGL.a is a linker script for its merged EGL/GL/NVK archive, -lmesa_local)
MESA_LIBS	:=	$(SWITCH2_MESA_SDK)/lib/libEGL.a
LIBS	:=	-lSDL2_ttf -lharfbuzz -lfreetype -lbz2 -lSDL2_image -lpng16 -ljpeg -lwebp -lSDL2 \
			$(MESA_LIBS) -lcurl -lzstd -lexpat -lz -lstdc++ -lm -lpthread -lkaihen -lnx

LIBDIRS	:= $(SWITCH2_MESA_SDK) $(PORTLIBS) $(LIBNX)

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

# Link with the C++ driver.
export LD	:=	$(CXX)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export APP_ICON := $(TOPDIR)/$(ICON)
export NROFLAGS += --icon=$(APP_ICON) --nacp=$(CURDIR)/$(TARGET).nacp --romfsdir=$(CURDIR)/$(ROMFS)

ifneq ($(APP_TITLEID),)
	export NACPFLAGS += --titleid=$(APP_TITLEID)
endif

# The romfs: romfs/ (logo, icons) and the cores, each with emu/<core>.sha256.
CORES		:=	$(wildcard $(CORE_DRAKO3D) $(CORE_VULKAN))
ifeq ($(strip $(CORES)),)
$(error "No ARMSX2 core to embed: build one first (switch/build-switch2.sh drako3d or vulkan)")
endif
ifeq ($(wildcard $(CORE_DRAKO3D)),)
$(warning "No Drako3D core ($(CORE_DRAKO3D)): this launcher will not start games with the Drako3D renderer")
endif
ifeq ($(wildcard $(CORE_VULKAN)),)
$(warning "No Vulkan core ($(CORE_VULKAN)): this launcher will not start games with the Vulkan renderer")
endif
STATIC_ROMFS	:=	$(patsubst romfs/%,$(ROMFS)/%,$(shell find romfs -type f))
STAGED_CORES	:=	$(foreach core,$(CORES),$(ROMFS)/emu/$(notdir $(core)))
STAGED_ROMFS	:=	$(STATIC_ROMFS) $(STAGED_CORES) $(STAGED_CORES:.nro=.sha256)

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD): $(STAGED_ROMFS)
	@rm -f $(filter-out $(STAGED_ROMFS),$(shell find $(ROMFS) -type f))
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(ROMFS)/%: romfs/%
	@mkdir -p $(@D)
	@cp -f $< $@

$(ROMFS)/emu/armsx2_drako3d.nro: $(CORE_DRAKO3D)
	@mkdir -p $(@D)
	@echo staging $(notdir $<)
	@cp -f $< $@

$(ROMFS)/emu/armsx2_vulkan.nro: $(CORE_VULKAN)
	@mkdir -p $(@D)
	@echo staging $(notdir $<)
	@cp -f $< $@

$(ROMFS)/emu/%.sha256: $(ROMFS)/emu/%.nro
	@sha256sum $< | cut -d' ' -f1 > $@

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

all	:	$(OUTPUT).nro

$(OUTPUT).nro	:	$(OUTPUT).elf $(OUTPUT).nacp

ROMFS_FILES := $(shell find $(TOPDIR)/$(ROMFS) -type f)
$(OUTPUT).nro : $(APP_ICON) $(ROMFS_FILES) $(TOPDIR)/Makefile

$(MESA_LIBS):
	@test -f "$@" || { echo "Missing $@: switch/build-switch2.sh vulkan prepares the Switch 2 Mesa SDK" >&2; exit 1; }

$(OUTPUT).elf	:	$(OFILES) $(MESA_LIBS)

$(OFILES_SRC)	: $(HFILES_BIN) $(TOPDIR)/Makefile

# (outside LTO: its definitions replace the wrapped libnx and newlib functions)
nx2_platform.o	:	CFLAGS += -fno-lto

%.bin.o	%_bin.h :	%.bin
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

endif
