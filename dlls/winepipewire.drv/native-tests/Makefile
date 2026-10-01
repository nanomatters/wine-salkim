# Standalone native tests. These do not connect to an audio server.
CC ?= cc
PKG_CONFIG ?= pkg-config
BUILD_DIR ?= build
WINE_BUILD_DIR ?= ../../..
CPPFLAGS += -D_GNU_SOURCE -D__WINESRC__ -DWINE_UNIX_LIB -DWINE_NO_TRACE_MSGS -DWINE_NO_DEBUG_MSGS -I../../../include
CPPFLAGS += -I$(WINE_BUILD_DIR)/include
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libpipewire-0.3)
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags libudev 2>/dev/null)
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fshort-wchar -ffunction-sections -fdata-sections -UNDEBUG
LDFLAGS += -Wl,--gc-sections
LDLIBS += $(shell $(PKG_CONFIG) --libs libpipewire-0.3)
LDLIBS += $(shell $(PKG_CONFIG) --libs libudev 2>/dev/null)
TESTS = identity plugins lifecycle stream probe

.PHONY: all check
all: $(addprefix $(BUILD_DIR)/,$(TESTS))

$(BUILD_DIR)/%: %.c.test test.h ../pipewire.c ../mult.h ../../../dlls/mmdevapi/unixlib.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c $< -x none $(LDFLAGS) $(LDLIBS) -o $@

check: all
	@set -e; for test in $(TESTS); do $(BUILD_DIR)/$$test; done
