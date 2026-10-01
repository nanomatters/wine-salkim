# Native implementation tests. No Wine runtime or audio server is needed.
CC ?= cc
BUILD_DIR ?= build
WINE_BUILD_DIR ?= ../../..
SPATIAL_SOURCE ?= ../spatialaudio.c
CPPFLAGS += -D_GNU_SOURCE -D__WINESRC__ -DWINE_UNIX_LIB -DWINE_NO_TRACE_MSGS -DWINE_NO_DEBUG_MSGS
CPPFLAGS += -I.. -I../../../include -I$(WINE_BUILD_DIR)/include
CPPFLAGS += -DSPATIAL_TEST_SOURCE='"$(SPATIAL_SOURCE)"'
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -fshort-wchar -ffunction-sections -fdata-sections -UNDEBUG
override LDFLAGS += -Wl,--gc-sections
LDLIBS += -pthread

.PHONY: all check
all: $(BUILD_DIR)/spatial

$(BUILD_DIR)/spatial: spatial.c.test $(SPATIAL_SOURCE) ../mmdevapi_private.h ../unixlib.h Makefile
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c $< -x none $(LDFLAGS) $(LDLIBS) -o $@

check: all
	$(BUILD_DIR)/spatial
