# Standalone native Linux tests; not part of Wine's Windows test suite.
CC ?= cc
BUILD_DIR ?= build
CPPFLAGS += -D_GNU_SOURCE -D__WINESRC__ -I../../../include
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -UNDEBUG

.PHONY: all check reader-check
all: $(BUILD_DIR)/dmabuf

$(BUILD_DIR)/dmabuf: dmabuf.c.test ../dmabuf.h ../../../include/wine/hwnd_dmabuf.h ../../../include/wine/server_protocol.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c dmabuf.c.test -x none $(LDFLAGS) -o $@

check: $(BUILD_DIR)/dmabuf reader-check
	$(BUILD_DIR)/dmabuf

reader-check:
	python3 reader-startup.py --build-dir $(BUILD_DIR)/reader --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'
