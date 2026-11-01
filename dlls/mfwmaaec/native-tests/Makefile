PYTHON ?= python3
BUILD_DIR ?= build

.PHONY: check sanitize
check:
	$(PYTHON) run.py --output $(BUILD_DIR)

sanitize:
	$(PYTHON) run.py --output $(BUILD_DIR)/sanitize --sanitize
