# Native tests include the production waveform implementation.
CC ?= cc
BUILD_DIR ?= build
WINE_BUILD_DIR ?= ../../..
WAVEFORM_SOURCE ?= ../waveform.c
CPPFLAGS += -D_GNU_SOURCE -D__WINESRC__ -DWINE_UNIX_LIB -DWINE_NO_TRACE_MSGS -DWINE_NO_DEBUG_MSGS
CPPFLAGS += -I.. -I../../../include -I$(WINE_BUILD_DIR)/include
CPPFLAGS += -DWAVEFORM_TEST_SOURCE='"$(WAVEFORM_SOURCE)"'
CFLAGS ?= -O2 -g
override CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format -Wno-sign-compare
override CFLAGS += -Wno-missing-field-initializers -fshort-wchar -ffunction-sections -fdata-sections -UNDEBUG
override LDFLAGS += -Wl,--gc-sections
LDLIBS += -pthread

.PHONY: all check
all: $(BUILD_DIR)/waveform-init $(BUILD_DIR)/waveform-lifetime $(BUILD_DIR)/waveform-mapping $(BUILD_DIR)/waveform-retry

$(BUILD_DIR)/waveform-%: waveform-%.c.test waveform.h $(WAVEFORM_SOURCE) Makefile
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c $< -x none $(LDFLAGS) $(LDLIBS) -o $@

check: all
	$(BUILD_DIR)/waveform-init
	$(BUILD_DIR)/waveform-lifetime
	$(BUILD_DIR)/waveform-mapping
	$(BUILD_DIR)/waveform-retry
