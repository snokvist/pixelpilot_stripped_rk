CC ?= gcc

# Default to using 4 parallel jobs unless the caller already requested a
# specific level of parallelism (e.g. via `make -j8`).
ifeq ($(filter -j%,$(MAKEFLAGS)),)
MAKEFLAGS += -j4
endif
PKG_CONFIG ?= pkg-config
PKG_DRMCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags libdrm)
PKG_DRMLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs libdrm)
PKG_GSTCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
PKG_GSTLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs gstreamer-1.0 gstreamer-video-1.0 gstreamer-app-1.0)
PKG_MPPCFLAGS := $(shell $(PKG_CONFIG) --silence-errors --cflags rockchip-mpp)
PKG_MPPLIBS := $(shell $(PKG_CONFIG) --silence-errors --libs rockchip-mpp)

CFLAGS ?= -O2 -Wall
CFLAGS += -Iinclude -Ithird_party/minimp4

# Allow opt-in control over NEON usage while auto-detecting safe defaults for
# supported ARM toolchains. Users can override by invoking `make ENABLE_NEON=0`
# or `make ENABLE_NEON=1`.
ENABLE_NEON ?= auto
CC_MACHINE := $(strip $(shell $(CC) -dumpmachine 2>/dev/null))
ifeq ($(CC_MACHINE),)
CC_MACHINE := $(strip $(shell uname -m 2>/dev/null))
endif

NEON_CFLAGS :=
NEON_DISABLE_DEFINE :=
NEON_ENABLED := 0

ifeq ($(ENABLE_NEON),0)
NEON_DISABLE_DEFINE := -DPIXELPILOT_DISABLE_NEON
else
  ifeq ($(ENABLE_NEON),1)
NEON_CFLAGS := -mfpu=neon -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
  else ifeq ($(ENABLE_NEON),auto)
    ifneq (,$(findstring aarch64,$(CC_MACHINE)))
NEON_CFLAGS := -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
    endif
    ifneq (,$(findstring arm64,$(CC_MACHINE)))
NEON_CFLAGS := -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
    endif
    ifneq (,$(findstring armv7,$(CC_MACHINE)))
NEON_CFLAGS := -mfpu=neon -DPIXELPILOT_HAS_NEON
NEON_ENABLED := 1
    endif
  endif
endif

CFLAGS += $(NEON_CFLAGS) $(NEON_DISABLE_DEFINE)
ifeq ($(strip $(PKG_DRMCFLAGS)),)
CFLAGS += -I/usr/include/libdrm
else
CFLAGS += $(PKG_DRMCFLAGS)
endif

ifneq ($(strip $(PKG_GSTCFLAGS)),)
CFLAGS += $(PKG_GSTCFLAGS)
endif

ifneq ($(strip $(PKG_MPPCFLAGS)),)
CFLAGS += $(PKG_MPPCFLAGS)
else
CFLAGS += -I/usr/include/rockchip
endif

ifneq ($(strip $(PKG_DRMLIBS)),)
LDFLAGS += $(PKG_DRMLIBS)
else
LDFLAGS += -ldrm
endif

ifneq ($(strip $(PKG_GSTLIBS)),)
LDFLAGS += $(PKG_GSTLIBS)
else
LDFLAGS += -lgstreamer-1.0 -lgstvideo-1.0 -lgstapp-1.0 -lgobject-2.0 -lglib-2.0
endif

ifneq ($(strip $(PKG_MPPLIBS)),)
LDFLAGS += $(PKG_MPPLIBS)
else
LDFLAGS += -lrockchip_mpp
endif

LDFLAGS += -lpthread
LDFLAGS += -ldl

SRC := $(wildcard src/*.c)
OBJ := $(SRC:.c=.o)
TARGET := pixelpilot_stripped_rk

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(OBJ) -o $@ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all clean
