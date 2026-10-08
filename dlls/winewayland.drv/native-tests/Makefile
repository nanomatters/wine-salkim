# Standalone native Linux tests; not part of Wine's Windows test suite.
CC ?= cc
BUILD_DIR ?= build
CPPFLAGS += -D_GNU_SOURCE -D__WINESRC__ -I../../../include
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -UNDEBUG

.PHONY: all check reader-check monitor-check
all: $(BUILD_DIR)/dmabuf

$(BUILD_DIR)/dmabuf: dmabuf.c.test ../dmabuf.h ../../../include/wine/hwnd_dmabuf.h ../../../include/wine/server_protocol.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) -x c dmabuf.c.test -x none $(LDFLAGS) -o $@

check: $(BUILD_DIR)/dmabuf reader-check monitor-check
	$(BUILD_DIR)/dmabuf

reader-check:
	python3 reader-startup.py --build-dir $(BUILD_DIR)/reader --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'

monitor-check:
	python3 monitor-layout.py --build-dir $(BUILD_DIR)/startup-layout --cc '$(CC)' \
	    --cflags '$(CFLAGS)'
	python3 display-cache.py --build-dir $(BUILD_DIR)/display-cache --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'
	python3 display-publication.py --build-dir $(BUILD_DIR)/display-publication --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'
	python3 display-notification.py --build-dir $(BUILD_DIR)/display-notification --cc '$(CC)' \
	    --cflags '$(CFLAGS)'
	python3 output-layout.py --build-dir $(BUILD_DIR)/output-layout --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'
	python3 fullscreen-outputs.py --build-dir $(BUILD_DIR)/fullscreen --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'
	python3 monitor-outputs.py --build-dir $(BUILD_DIR)/monitor --cc '$(CC)' \
	    --cflags '$(CFLAGS) -Wno-unused-parameter'
