# Moonlight webOS Port Makefile
# Cross-compilation for HP TouchPad (webOS 3.0.5)

# Toolchain
PDK ?= /opt/PalmPDK
CC = $(PDK)/arm-toolchain/bin/arm-none-linux-gnueabi-gcc
AR = $(PDK)/arm-toolchain/bin/arm-none-linux-gnueabi-ar
STRIP = $(PDK)/arm-toolchain/bin/arm-none-linux-gnueabi-strip

# Paths (override on the command line: make SYSROOT=... MOONLIGHT_EMBEDDED=...)
# SYSROOT holds the cross-built static deps (FFmpeg, curl, OpenSSL, expat, opus)
SYSROOT ?= $(CURDIR)/sysroot
# MOONLIGHT_EMBEDDED is a recursive checkout of moonlight-embedded
MOONLIGHT_EMBEDDED ?= $(CURDIR)/deps/moonlight-embedded
MOONLIGHT_COMMON = $(MOONLIGHT_EMBEDDED)/third_party/moonlight-common-c
ENET = $(MOONLIGHT_COMMON)/enet
REED_SOLOMON = $(MOONLIGHT_COMMON)/reedsolomon
LIBGAMESTREAM = $(MOONLIGHT_EMBEDDED)/libgamestream
H264_BITSTREAM = $(MOONLIGHT_EMBEDDED)/third_party/h264bitstream

# Build directory
BUILD = build

# Compiler flags
CFLAGS = -std=gnu99 -O2 -march=armv7-a -mfpu=neon -mfloat-abi=softfp
CFLAGS += -Wall -Wno-unused-parameter -Wno-sign-compare
CFLAGS += -I$(SYSROOT)/include
CFLAGS += -I$(PDK)/include
CFLAGS += -I$(PDK)/include/SDL
# Use our patched headers first
CFLAGS += -Iinclude
CFLAGS += -I$(MOONLIGHT_COMMON)/src
CFLAGS += -I$(MOONLIGHT_COMMON)/enet/include
CFLAGS += -I$(MOONLIGHT_COMMON)/reedsolomon
CFLAGS += -I$(LIBGAMESTREAM)
CFLAGS += -I$(H264_BITSTREAM)
CFLAGS += -Isrc
# FFmpeg includes
CFLAGS += -I$(SYSROOT)/include

# Defines for webOS port
CFLAGS += -DHAVE_SOCKLEN_T
CFLAGS += -D_GNU_SOURCE
CFLAGS += -DNO_AVAHI
CFLAGS += -DWEBOS


# Linker flags - static link our deps, dynamic link PDK libs
LDFLAGS = -L$(SYSROOT)/lib
LDFLAGS += -L$(PDK)/device/lib
# Include toolchain libc for static pthread
LDFLAGS += -L$(PDK)/arm-toolchain/arm-none-linux-gnueabi/libc/usr/lib
LDFLAGS += -Wl,--allow-shlib-undefined

# Libraries - order matters for static linking
# Static link our deps (curl before ssl since curl uses ssl)
LIBS = -Wl,-Bstatic -lavcodec -lavformat -lavutil -lcurl -lssl -lcrypto -lexpat -lopus
LIBS += -Wl,-Bdynamic -lSDL -lSDL_ttf -lSDL_mixer -lpdl -lGLESv2
LIBS += -lpthread -lrt -ldl -lm -lz

# moonlight-common-c sources
COMMON_SRC = $(wildcard $(MOONLIGHT_COMMON)/src/*.c)
COMMON_OBJ = $(patsubst $(MOONLIGHT_COMMON)/src/%.c,$(BUILD)/common/%.o,$(COMMON_SRC))

# enet sources
ENET_SRC = $(wildcard $(ENET)/*.c)
ENET_OBJ = $(patsubst $(ENET)/%.c,$(BUILD)/enet/%.o,$(ENET_SRC))

# reedsolomon sources
RS_SRC = $(wildcard $(REED_SOLOMON)/*.c)
RS_OBJ = $(patsubst $(REED_SOLOMON)/%.c,$(BUILD)/rs/%.o,$(RS_SRC))

# libgamestream sources
GS_SRC = $(LIBGAMESTREAM)/client.c \
         $(LIBGAMESTREAM)/http.c \
         $(LIBGAMESTREAM)/mkcert.c \
         $(LIBGAMESTREAM)/xml.c \
         $(LIBGAMESTREAM)/sps.c
GS_OBJ = $(patsubst $(LIBGAMESTREAM)/%.c,$(BUILD)/gs/%.o,$(GS_SRC))

# h264bitstream sources
H264_SRC = $(H264_BITSTREAM)/h264_nal.c \
           $(H264_BITSTREAM)/h264_stream.c \
           $(H264_BITSTREAM)/h264_sei.c
H264_OBJ = $(patsubst $(H264_BITSTREAM)/%.c,$(BUILD)/h264/%.o,$(H264_SRC))

# webOS port sources
WEBOS_SRC = $(wildcard src/*.c)
WEBOS_OBJ = $(patsubst src/%.c,$(BUILD)/webos/%.o,$(WEBOS_SRC))

# All objects
ALL_OBJ = $(COMMON_OBJ) $(ENET_OBJ) $(RS_OBJ) $(GS_OBJ) $(H264_OBJ) $(WEBOS_OBJ)

# Target
TARGET = moonlight

.PHONY: all check clean dirs

all: check dirs $(TARGET)

check:
	@test -x $(CC) || { echo "Cross-compiler not found at $(CC); set PDK=..."; exit 1; }
	@test -d $(MOONLIGHT_COMMON)/src || { echo "moonlight-embedded not found at $(MOONLIGHT_EMBEDDED); set MOONLIGHT_EMBEDDED=... (see README)"; exit 1; }
	@test -d $(SYSROOT)/lib || { echo "Dependency sysroot not found at $(SYSROOT); set SYSROOT=... (see README)"; exit 1; }

dirs:
	@mkdir -p $(BUILD)/common
	@mkdir -p $(BUILD)/enet
	@mkdir -p $(BUILD)/rs
	@mkdir -p $(BUILD)/gs
	@mkdir -p $(BUILD)/h264
	@mkdir -p $(BUILD)/webos
	@mkdir -p $(BUILD)/common-src

$(TARGET): $(ALL_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(STRIP) $@

# Pattern rules
# moonlight-common-c is compiled from a copy with our patched headers laid
# over it: its sources include their headers from their own directory, so
# -Iinclude alone would not reach them.
$(BUILD)/common/%.o: $(MOONLIGHT_COMMON)/src/%.c $(wildcard include/*.h)
	@cp $(MOONLIGHT_COMMON)/src/*.c $(MOONLIGHT_COMMON)/src/*.h $(BUILD)/common-src/
	@cp include/*.h $(BUILD)/common-src/
	$(CC) $(CFLAGS) -c -o $@ $(BUILD)/common-src/$*.c

$(BUILD)/enet/%.o: $(ENET)/%.c
	$(CC) $(CFLAGS) -I$(ENET)/include -c -o $@ $<

$(BUILD)/rs/%.o: $(REED_SOLOMON)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/gs/%.o: $(LIBGAMESTREAM)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/h264/%.o: $(H264_BITSTREAM)/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/webos/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -rf $(BUILD) $(TARGET)

# Package for webOS
package: $(TARGET)
	@mkdir -p staging
	cp $(TARGET) staging/
	cp appinfo.json staging/
	cp assets/FreeSans.ttf staging/font.ttf
	cp icon.png staging/ 2>/dev/null || echo "Warning: no icon.png"
	echo "filemode.755=$(TARGET)" > staging/package.properties
	palm-package staging
	rm -rf staging
