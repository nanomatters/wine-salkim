# Standalone native Linux tests; not part of Wine's Windows test suite.
CC ?= cc
BUILD_DIR ?= build
CPPFLAGS += -D_GNU_SOURCE -D__WINESRC__ -I../../../include
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -UNDEBUG

.PHONY: all check
all: $(BUILD_DIR)/dmabuf

$(BUILD_DIR)/dmabuf: dmabuf.c ../dmabuf.h ../../../include/wine/hwnd_dmabuf.h ../../../include/wine/server_protocol.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) dmabuf.c $(LDFLAGS) -o $@

check: $(BUILD_DIR)/dmabuf
	$(BUILD_DIR)/dmabuf
