CC ?= cc
AR ?= ar
CFLAGS ?= -O2
REPO_ROOT ?= ../../..
BUILD_DIR ?= build

COMMON_FLAGS := -std=c11 -Wall -Wextra -Werror -D_DEFAULT_SOURCE \
	-Iinclude -I$(REPO_ROOT)/libs/pal/include
TEST_CPPFLAGS := -DH2_LINUX_TESTING
SOURCES := src/h2_linux_evdev_button.c src/h2_linux_evdev_touch.c \
	src/h2_linux_gpio_button.c \
	src/h2_linux_platform.c src/h2_linux_sync.c
OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SOURCES))

.PHONY: all clean test

all: $(BUILD_DIR)/libh2_linux_platform.a

$(BUILD_DIR):
	mkdir -p $@

$(BUILD_DIR)/%.o: src/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(COMMON_FLAGS) -c $< -o $@

$(BUILD_DIR)/libh2_linux_platform.a: $(OBJECTS)
	$(AR) rcs $@ $^

test: | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(COMMON_FLAGS) $(TEST_CPPFLAGS) \
		src/h2_linux_platform.c tests/test_linux_platform.c -pthread \
		-o $(BUILD_DIR)/test_linux_platform
	$(BUILD_DIR)/test_linux_platform
	$(CC) $(CFLAGS) $(COMMON_FLAGS) $(TEST_CPPFLAGS) \
		src/h2_linux_evdev_button.c tests/test_linux_evdev_button.c \
		-o $(BUILD_DIR)/test_linux_evdev_button
	$(BUILD_DIR)/test_linux_evdev_button
	$(CC) $(CFLAGS) $(COMMON_FLAGS) $(TEST_CPPFLAGS) \
		src/h2_linux_evdev_touch.c tests/test_linux_evdev_touch.c \
		-o $(BUILD_DIR)/test_linux_evdev_touch
	$(BUILD_DIR)/test_linux_evdev_touch
	$(CC) $(CFLAGS) $(COMMON_FLAGS) $(TEST_CPPFLAGS) \
		src/h2_linux_gpio_button.c tests/test_linux_gpio_button.c \
		-o $(BUILD_DIR)/test_linux_gpio_button
	$(BUILD_DIR)/test_linux_gpio_button
	$(CC) $(CFLAGS) $(COMMON_FLAGS) \
		src/h2_linux_sync.c tests/test_linux_sync.c -pthread \
		-o $(BUILD_DIR)/test_linux_sync
	$(BUILD_DIR)/test_linux_sync

clean:
	rm -f $(OBJECTS) $(BUILD_DIR)/libh2_linux_platform.a \
		$(BUILD_DIR)/test_linux_platform \
		$(BUILD_DIR)/test_linux_evdev_button \
		$(BUILD_DIR)/test_linux_evdev_touch \
		$(BUILD_DIR)/test_linux_gpio_button \
		$(BUILD_DIR)/test_linux_sync
