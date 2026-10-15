CC ?= cc
BUILD_DIR ?= build
WINE_BUILD_DIR ?= ../../..
CPPFLAGS += -D__WINESRC__ -DWINE_NO_TRACE_MSGS -DWINE_NO_DEBUG_MSGS -I../../../include
CPPFLAGS += -I$(WINE_BUILD_DIR)/include -I$(WINE_BUILD_DIR)/dlls/ntoskrnl.exe
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-format -Wno-missing-field-initializers
CFLAGS += -Wno-sign-compare -fshort-wchar
CFLAGS += -ffunction-sections -fdata-sections -fno-builtin -UNDEBUG
LDFLAGS += -Wl,--gc-sections

.PHONY: all check
all: $(BUILD_DIR)/pnp_ids

$(BUILD_DIR)/pnp_ids: pnp_ids.c.test ../pnp.c ../ntoskrnl_private.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c $< -x none $(LDFLAGS) -o $@

check: all
	$(BUILD_DIR)/pnp_ids
