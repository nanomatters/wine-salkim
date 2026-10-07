# Standalone native Linux tests. Not part of Wine's Windows test suite.
CC ?= cc
BUILD_DIR ?= build
WINE_BUILD_DIR ?= ../../..
GENERATED_INCLUDE_DIR ?= $(WINE_BUILD_DIR)/include
CPPFLAGS += -D__WINESRC__ -I../../../include -I$(GENERATED_INCLUDE_DIR) -I..
CFLAGS ?= -O2 -g
TEST_CFLAGS = -std=gnu11 -Wall -Werror=implicit-function-declaration -UNDEBUG \
    -fshort-wchar -ffunction-sections -fdata-sections
TEST_LDFLAGS = -Wl,--gc-sections

.PHONY: all check
all: $(BUILD_DIR)/read_thread $(BUILD_DIR)/video_padding

$(BUILD_DIR)/read_thread: read_thread.c ../quartz_parser.c ../gst_private.h ../unixlib.h \
    $(GENERATED_INCLUDE_DIR)/strmif.h $(GENERATED_INCLUDE_DIR)/amvideo.h \
    $(GENERATED_INCLUDE_DIR)/mfidl.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) read_thread.c \
	    $(LDFLAGS) $(TEST_LDFLAGS) -o $@

$(BUILD_DIR)/video_padding: video_padding.c ../wg_transform.c ../unix_private.h ../unixlib.h
	mkdir -p $(BUILD_DIR)
	$(CC) $(CPPFLAGS) -I$(WINE_BUILD_DIR) $(CFLAGS) $(TEST_CFLAGS) $(shell pkg-config --cflags gstreamer-video-1.0 gstreamer-audio-1.0) \
	    video_padding.c $(LDFLAGS) $(TEST_LDFLAGS) $(shell pkg-config --libs gstreamer-video-1.0 gstreamer-audio-1.0) -o $@

check: $(BUILD_DIR)/read_thread $(BUILD_DIR)/video_padding
	$(BUILD_DIR)/read_thread
	$(BUILD_DIR)/video_padding
