REPO_ROOT := $(abspath ../../../../..)
BK3633_SDK_ROOT := $(abspath $(BK3633_PATH)/SDK)
SDK_PROJECT := $(BK3633_SDK_ROOT)/projects/app_gatt_all_roles
SDK_BIM := $(SDK_PROJECT)/output/boot/BK3633_BIM_ALLROLES.bin
SDK_STACK := $(SDK_PROJECT)/output/stack/BK3633_STACK_ALLROLES.bin

ifndef H2_BAZEL_COMPONENT_MANIFEST
$(error H2_BAZEL_COMPONENT_MANIFEST is required)
endif
include $(H2_BAZEL_COMPONENT_MANIFEST)

BUILD_DIR ?= build
RELEASE_DIR ?= $(REPO_ROOT)/zig-out/boards/bk3633_chip
TARGET := bk3633_chip
IMAGE_ID := reference-smoke
MERGE_NAME := bk3633_chip_merge_crc.bin
TOOLCHAIN_PREFIX := $(abspath $(COMPILER_TOOLCHAIN_PATH))/arm-none-eabi-
CC := $(TOOLCHAIN_PREFIX)gcc
OBJCOPY := $(TOOLCHAIN_PREFIX)objcopy
NM := $(TOOLCHAIN_PREFIX)nm
TOOLCHAIN_VERSION := $(shell $(CC) -dumpfullversion 2>/dev/null)
EXPECTED_TOOLCHAIN_VERSION := 10.3.1
ELF := $(abspath $(BUILD_DIR)/$(TARGET).elf)
APP_BIN := $(abspath $(BUILD_DIR)/$(TARGET).bin)
MAP := $(abspath $(BUILD_DIR)/$(TARGET).map)
NATIVE_OBJECTS := $(foreach source,$(H2_BAZEL_NATIVE_SRCS),$(abspath $(BUILD_DIR)/$(notdir $(basename $(source))).o))
NATIVE_STAMP := $(abspath $(BUILD_DIR)/native-components.stamp)
MERGE_INPUT_DIR := $(abspath $(BUILD_DIR)/merge-input)
MERGE_OUTPUT := $(MERGE_INPUT_DIR)/app_merge_crc.bin
FINAL_MERGE := $(abspath $(RELEASE_DIR)/$(MERGE_NAME))
MANIFEST := $(abspath $(RELEASE_DIR)/manifest.json)

.PHONY: sdk_probe release

sdk_probe:
	@test -d "$(SDK_PROJECT)" || (echo "missing BK3633 SDK build substrate"; exit 1)
	@test -f "$(SDK_BIM)" || (echo "missing BK3633 BIM"; exit 1)
	@test -f "$(SDK_STACK)" || (echo "missing BK3633 allroles stack"; exit 1)
	@test -x "$(CC)" || (echo "missing BK3633 compiler"; exit 1)
	@test "$(TOOLCHAIN_VERSION)" = "$(EXPECTED_TOOLCHAIN_VERSION)" || \
		(echo "BK ARM toolchain mismatch: expected $(EXPECTED_TOOLCHAIN_VERSION), found $(TOOLCHAIN_VERSION)"; exit 1)
	@test -n "$(H2_BAZEL_ARCHIVES)" || (echo "firmware library archives are required"; exit 1)

$(NATIVE_STAMP): $(H2_BAZEL_NATIVE_SRCS)
	@mkdir -p "$(BUILD_DIR)"
	$(foreach source,$(H2_BAZEL_NATIVE_SRCS),$(CC) -Os -mcpu=arm968e-s -march=armv5te -mthumb -mthumb-interwork -flto -ffunction-sections -fdata-sections $(H2_BAZEL_NATIVE_INCLUDES) -c "$(source)" -o "$(abspath $(BUILD_DIR)/$(notdir $(basename $(source))).o)" &&) true
	@touch "$@"

$(ELF): $(NATIVE_STAMP)
	@mkdir -p "$(BUILD_DIR)"
	$(MAKE) -C "$(SDK_PROJECT)" \
		BUILD_DIR="$(abspath $(BUILD_DIR))" \
		TARGET="$(TARGET)" \
		CC="$(CC)" AR="$(TOOLCHAIN_PREFIX)gcc-ar" \
		LKLIBS="$(NATIVE_OBJECTS) -Wl,--start-group $(H2_BAZEL_ARCHIVES) -Wl,--end-group" \
		"$(ELF)"

$(APP_BIN): $(ELF)
	$(OBJCOPY) -O binary "$<" "$@"

$(MAP): $(ELF)
	$(NM) -n "$<" > "$@"

release: sdk_probe $(APP_BIN) $(MAP)
	@test -n "$(BINCONVERT)" && test -x "$(BINCONVERT)"
	@mkdir -p "$(MERGE_INPUT_DIR)" "$(RELEASE_DIR)"
	cp "$(SDK_BIM)" "$(MERGE_INPUT_DIR)/bim.bin"
	cp "$(SDK_STACK)" "$(MERGE_INPUT_DIR)/stack.bin"
	cp "$(APP_BIN)" "$(MERGE_INPUT_DIR)/app.bin"
	"$(BINCONVERT)" -oad "$(MERGE_INPUT_DIR)/bim.bin" \
		"$(MERGE_INPUT_DIR)/stack.bin" "$(MERGE_INPUT_DIR)/app.bin" \
		-m 0x1F00 -l 0x2E200 -v 0x0022 -rom_v 0x000F \
		-e 00000000 00000000 00000000 00000000
	@test -s "$(MERGE_OUTPUT)"
	cp "$(ELF)" "$(RELEASE_DIR)/$(TARGET).elf"
	cp "$(APP_BIN)" "$(RELEASE_DIR)/$(TARGET).bin"
	cp "$(MAP)" "$(RELEASE_DIR)/$(TARGET).map"
	cp "$(MERGE_OUTPUT)" "$(FINAL_MERGE)"
	@printf '%s\n' \
		'{' \
		'  "target": "bk3633",' \
		'  "board": "bk3633_chip",' \
		'  "image": "$(IMAGE_ID)",' \
		'  "stack": "allroles",' \
		'  "runtime": "baremetal_ringbuf",' \
		'  "toolchain": "arm-none-eabi-gcc $(TOOLCHAIN_VERSION)",' \
		'  "binconverter": "portable-c",' \
		'  "elf": "$(TARGET).elf",' \
		'  "bin": "$(TARGET).bin",' \
		'  "map": "$(TARGET).map",' \
		'  "persistent_layout": {"source": "bk3633_chip"},' \
		'  "merge": "$(MERGE_NAME)",' \
		'  "merge_inputs": ["bim.bin", "stack.bin", "app.bin"]' \
		'}' > "$(MANIFEST)"
