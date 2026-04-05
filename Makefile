#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#
# NO_SMDH: if set to anything, no SMDH file is created.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libnds default>
#---------------------------------------------------------------------------------
TARGET      :=  $(notdir $(CURDIR))
BUILD       :=  build
SOURCES     :=  source libs
DATA        :=  data
INCLUDES    :=  include libs
GRAPHICS    :=  gfx
GFXBUILD    :=  $(BUILD)
ROMFS       :=  romfs

APP_TITLE   :=  Kavita 3DS
APP_DESCRIPTION :=  Kavita comic/manga reader
APP_AUTHOR  :=  kavita-3ds

# smdhtool requires 48x48; source icon.png may be smaller — reuse CIA prep output.
ICON        :=  $(BUILD)/cia_icon48.png

# CIA packaging (makerom + bannertool on PATH, or set MAKEROM / BANNERTOOL; needs mkromfs3ds from devkitPro tools)
CIA_RSF         :=  cia/kavita-3ds.rsf
CIA_BANNER_PNG  :=  $(BUILD)/cia_banner.png
CIA_ICON48_PNG  :=  $(BUILD)/cia_icon48.png
CIA_SILENT_WAV  :=  $(BUILD)/cia_silent.wav
CIA_BANNER_BIN  :=  $(BUILD)/cia_banner.bin
CIA_SMDH        :=  $(BUILD)/cia_icon.smdh
PYTHON          ?=  python
# Prefer unpacked tools under .cia-tools (Windows .exe; paths work under MSYS2 make).
CIA_MAKEROM_LOCAL    :=  $(CURDIR)/.cia-tools/makerom/makerom.exe
CIA_BANNERTOOL_LOCAL :=  $(CURDIR)/.cia-tools/bannertool/bannertool-1.2.2-windows/bannertool.exe
ifeq ($(wildcard $(CIA_MAKEROM_LOCAL)),)
BANNERTOOL      ?=  bannertool
MAKEROM         ?=  makerom
else
BANNERTOOL      ?=  $(CIA_BANNERTOOL_LOCAL)
MAKEROM         ?=  $(CIA_MAKEROM_LOCAL)
endif
SMDHTOOL        ?=  smdhtool

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH    :=  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS  :=  -g -Wall -O2 -mword-relocations \
            -fomit-frame-pointer -ffunction-sections \
            -pipe \
            $(ARCH)

CFLAGS  +=  $(INCLUDE) -D__3DS__

CXXFLAGS    := $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS :=  -g $(ARCH)
LDFLAGS  =  -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS    :=  -Wl,--start-group -lcitro2d -lcitro3d -lctru -lmbedtls -lmbedx509 -lmbedcrypto -Wl,--end-group -lz -lm

#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS := $(DEVKITPRO)/libctru $(PORTLIBS)


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   :=  $(CURDIR)/$(TARGET)
export TOPDIR   :=  $(CURDIR)

export VPATH    :=  $(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
                    $(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir))

export DEPSDIR  :=  $(CURDIR)/$(BUILD)

CFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES   :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES :=  $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES    :=  $(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES    :=  $(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
    export LD   :=  $(CC)
else
    export LD   :=  $(CXX)
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES   :=  $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN   :=  $(addsuffix .o,$(BINFILES)) \
                        $(PICAFILES:.v.pica=.shbin.o) \
                        $(SHLISTFILES:.shlist=.shbin.o) \
                        $(addsuffix .o,$(GFXFILES))

export OFILES   :=  $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES   :=  $(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
                    $(addsuffix .h,$(subst .,_,$(BINFILES))) \
                    $(GFXFILES:.t3s=.h)

export INCLUDE  :=  $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                    $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                    -I$(CURDIR)/$(BUILD)

export LIBPATHS :=  $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)

ifeq ($(strip $(ICON)),)
    icons := $(wildcard *.png)
    ifneq (,$(findstring $(TARGET).png,$(icons)))
        export APP_ICON := $(TOPDIR)/$(TARGET).png
    else
        ifneq (,$(findstring icon.png,$(icons)))
            export APP_ICON := $(TOPDIR)/icon.png
        endif
    endif
else
    export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
    export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
    export _3DSXDEPS = $(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
    export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: $(BUILD) clean all cia

#---------------------------------------------------------------------------------
all: $(BUILD) $(CIA_ICON48_PNG)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

$(BUILD):
	@[ -d $@ ] || mkdir -p $@

#---------------------------------------------------------------------------------
# ELF is produced by the sub-make; outer targets (e.g. cia) need this explicit edge.
$(OUTPUT).elf: $(BUILD)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile $(OUTPUT).elf

#---------------------------------------------------------------------------------
$(CIA_BANNER_PNG) $(CIA_ICON48_PNG) $(CIA_SILENT_WAV): icon.png icon-large.png tools/prepare_cia_assets.py
	@$(PYTHON) tools/prepare_cia_assets.py --root $(CURDIR) --build $(CURDIR)/$(BUILD)

$(CIA_BANNER_BIN): $(CIA_BANNER_PNG) $(CIA_SILENT_WAV)
	$(BANNERTOOL) makebanner -i $(CIA_BANNER_PNG) -a $(CIA_SILENT_WAV) -o $@

$(CIA_SMDH): $(CIA_ICON48_PNG)
	$(SMDHTOOL) --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(CIA_ICON48_PNG) $@

# RomFS is built by makerom from RomFs:RootPath in the RSF (IVFC; do not use mkromfs3ds output with -romfs).
$(OUTPUT).cia: $(OUTPUT).elf $(CIA_BANNER_BIN) $(CIA_SMDH) $(CIA_RSF) $(sort $(wildcard $(ROMFS)/*))
	$(MAKEROM) -f cia -o $@ -rsf $(CIA_RSF) -target t -elf $(OUTPUT).elf -icon $(CIA_SMDH) -banner $(CIA_BANNER_BIN) -desc app:4 -exefslogo

cia: $(OUTPUT).cia

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(OUTPUT).smdh $(TARGET).elf $(TARGET).cia

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx   :   $(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)

$(OUTPUT).elf    :   $(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o %_bin.h :   %.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PHONY: all

#---------------------------------------------------------------------------------
# Compile rules
#---------------------------------------------------------------------------------
-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
