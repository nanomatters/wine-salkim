# Standalone native Linux tests. Not part of Wine's Windows test suite.
CC ?= cc
BUILD_DIR ?= build
CFLAGS ?= -O2 -g
TEST_CFLAGS = -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -UNDEBUG

.PHONY: all check
all: $(BUILD_DIR)/source_start

$(BUILD_DIR)/source_start.h: ../media_source.c
	mkdir -p $(BUILD_DIR)
	awk 'BEGIN { found=0; depth=0 } /^static HRESULT media_source_start\(/ { found=1 } \
	    found { print; for(i=1;i<=length($$0);i++) { c=substr($$0,i,1); if(c=="{") depth++; \
	    if(c=="}") { depth--; if(depth==0) exit } } }' $< > $@
	test -s $@

$(BUILD_DIR)/source_start: source_start.c $(BUILD_DIR)/source_start.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_CFLAGS) -I$(BUILD_DIR) $< $(LDFLAGS) -o $@

check: $(BUILD_DIR)/source_start
	$(BUILD_DIR)/source_start
