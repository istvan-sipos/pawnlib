CC      := i686-w64-mingw32-gcc
WINDRES := i686-w64-mingw32-windres
CFLAGS  := -O2 -Wall -Wextra -Wno-unused-parameter -static-libgcc -std=gnu99 -fno-omit-frame-pointer
LDFLAGS := -shared -Wl,--kill-at

VERSION := $(shell cat VERSION)
# "1.1.0" -> "1,1,0,0" for Windows VS_VERSION_INFO FILEVERSION (always 4
# components).  Embedded PE metadata is the main lever we have against the
# Wacatac.B!ml / Ti!*-class false-positive flags that hit unsigned MinGW
# binaries with empty version info.
comma   := ,
VER_NUM := $(subst .,$(comma),$(VERSION)),0
# windres re-invokes gcc -E -xc on the .rc, so the inner quotes need to
# survive a *second* shell-quoting pass.  '\"...\"' is what reaches cpp.
WINDRESFLAGS := -DPAWNLIB_VER_NUM=$(VER_NUM) '-DPAWNLIB_VER_STR=\"$(VERSION)\"' -O coff

TARGET := pawnlib.dll
SRC    := pawnlib.c pawnxfs.c pawnsave.c
RES    := pawnlib.res.o

# Under the new architecture pawnlib.dll sits next to DDDA.exe / dinput8.dll
# (loaded via ddda-dinput8's [main] loadLibrary= line), not under
# steam_settings/load_dlls/ as the old gbe_fork-coupled pawndb did.
DDDA_DIR := /home/istvan/.steam/steam/steamapps/common/DDDA
DEPLOY   := $(DDDA_DIR)/$(TARGET)

# Companion CLI tool — splices an archived pawn's <HEX>.xml sidecar into a
# DDDA.sav main-pawn slot.  Standalone, no Steam dependency; built on
# demand via `make tools`, not part of the default `all:` target.
TOOLS_TARGET := tools/restore_pawn.exe
TOOLS_SRC    := tools/restore_pawn.c pawnsave.c
TOOLS_RES    := tools/restore_pawn.res.o

.PHONY: all clean deploy deploy-ini tools

all: deploy

$(RES): pawnlib.rc VERSION
	$(WINDRES) $(WINDRESFLAGS) -i pawnlib.rc -o $@

$(TARGET): $(SRC) $(RES) VERSION
	$(CC) $(CFLAGS) -DPAWNLIB_VERSION='"$(VERSION)"' $(LDFLAGS) -o $@ $(SRC) $(RES) -luser32 -lkernel32 -lversion -Wl,-Bstatic -lz -Wl,-Bdynamic

deploy: $(TARGET)
	cp $(TARGET) "$(DEPLOY)"
	@echo "== deploy hash check =="
	@md5sum $(TARGET) "$(DEPLOY)"

# Copy the default pawnlib.ini next to the DLL.  Separate from `deploy` so a
# user-customised ini isn't clobbered on every build.  Run once on install,
# or any time you want to reset the config to defaults.
deploy-ini: pawnlib.ini
	cp pawnlib.ini "$(DDDA_DIR)/pawnlib.ini"
	@echo "== ini deployed to $(DDDA_DIR)/pawnlib.ini =="

$(TOOLS_RES): tools/restore_pawn.rc VERSION
	$(WINDRES) $(WINDRESFLAGS) -i tools/restore_pawn.rc -o $@

$(TOOLS_TARGET): $(TOOLS_SRC) $(TOOLS_RES) VERSION
	$(CC) $(CFLAGS) -DPAWNLIB_VERSION='"$(VERSION)"' -o $@ $(TOOLS_SRC) $(TOOLS_RES) -Wl,-Bstatic -lz -Wl,-Bdynamic

tools: $(TOOLS_TARGET)
	@echo "== built $(TOOLS_TARGET) =="

clean:
	rm -f $(TARGET) $(TOOLS_TARGET) $(RES) $(TOOLS_RES)
