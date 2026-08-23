REPO_ROOT := $(abspath ../../..)
BUILD_DIR ?= build/probes
HOST_CC ?= cc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror
BK3633_PATH ?=
BK3633_SDK_ROOT := $(if $(strip $(BK3633_PATH)),$(abspath $(BK3633_PATH)/SDK))
INCLUDES := -Iinclude \
	-I$(REPO_ROOT)/libs/drivers/rtc/bm8563/include \
	-I$(REPO_ROOT)/libs/pal/providers/libco/include \
	-I$(REPO_ROOT)/third_party/libco \
	-I$(REPO_ROOT)/libs/pal/include

.PHONY: tests probes ble_sdk_probe button_sdk_probe buzzer_sdk_probe \
	entropy_sdk_probe storage_sdk_probe battery_sdk_probe power_sdk_probe \
	time_sdk_probe libco_arm_probe clean

tests:
	cd $(REPO_ROOT) && bazel test \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_entropy \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_libco \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_mem \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_button \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_buzzer \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_battery \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_power \
		//native_component_src/bk3633/h2_pal_core:test_bk3633_platform_time \
		//native_component_src/bk3633/h2_pal_core:test_nvds_pref \
		//native_component_src/bk3633/h2_pal_core:test_flash_disk

probes: \
	$(BUILD_DIR)/mem_probe \
	$(BUILD_DIR)/log_probe \
	$(BUILD_DIR)/system_event_probe \
	$(BUILD_DIR)/firmware_info_probe
	$(BUILD_DIR)/mem_probe
	$(BUILD_DIR)/log_probe
	$(BUILD_DIR)/system_event_probe
	$(BUILD_DIR)/firmware_info_probe
	$(MAKE) ble_sdk_probe
	$(MAKE) button_sdk_probe
	$(MAKE) buzzer_sdk_probe
	$(MAKE) entropy_sdk_probe
	$(MAKE) storage_sdk_probe
	$(MAKE) battery_sdk_probe
	$(MAKE) power_sdk_probe
	$(MAKE) time_sdk_probe
	$(MAKE) libco_arm_probe
	@echo "BK3633 h2_pal_core probes OK"

ifeq ($(strip $(BK3633_SDK_ROOT)$(BK3633_CONFIG_INCLUDE_DIR)),)
ble_sdk_probe:
	@echo "BK3633 BLE SDK probe SKIP: BK3633_PATH and BK3633_CONFIG_INCLUDE_DIR are required"
else
ble_sdk_probe:
	cmake -S . -B $(BUILD_DIR)/sdk \
		-DBK3633_PATH=$(BK3633_PATH) \
		-DBK3633_CONFIG_INCLUDE_DIR=$(BK3633_CONFIG_INCLUDE_DIR)
	cmake --build $(BUILD_DIR)/sdk --target bk3633_ble_probe
endif

ifeq ($(strip $(COMPILER_TOOLCHAIN_PATH)),)
libco_arm_probe:
	@echo "BK3633 libco ARM probe SKIP: COMPILER_TOOLCHAIN_PATH is required"
else
libco_arm_probe:
	@mkdir -p $(BUILD_DIR)/libco-arm
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-gcc \
		-std=c11 -Wall -Wextra -Werror -Wno-unused-value -ffreestanding \
		-mcpu=arm968e-s -march=armv5te -marm -mthumb-interwork \
		-I$(REPO_ROOT)/libs/pal/providers/libco/include \
		-I$(REPO_ROOT)/third_party/libco \
		-c probes/src/libco_arm_probe.c \
		-o $(BUILD_DIR)/libco-arm/probe.o
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-gcc \
		-std=c11 -Wall -Wextra -Werror -Wno-unused-value -ffreestanding \
		-ffunction-sections -fdata-sections \
		-mcpu=arm968e-s -march=armv5te -marm -mthumb-interwork \
		-I$(REPO_ROOT)/third_party/libco \
		-c $(REPO_ROOT)/third_party/libco/libco.c \
		-o $(BUILD_DIR)/libco-arm/libco.o
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-nm \
		$(BUILD_DIR)/libco-arm/libco.o | grep -q ' T co_derive$$'
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-nm \
		$(BUILD_DIR)/libco-arm/libco.o | grep -q ' T co_switch$$'
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-readelf -h \
		$(BUILD_DIR)/libco-arm/libco.o | grep -q 'Class:.*ELF32'
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-readelf -h \
		$(BUILD_DIR)/libco-arm/libco.o | grep -q 'Machine:.*ARM'
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-readelf -h \
		$(BUILD_DIR)/libco-arm/libco.o | grep -q 'Version5 EABI'
	@! $(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-readelf -S \
		$(BUILD_DIR)/libco-arm/libco.o | grep -Eq '\.(tdata|tbss)'
	$(COMPILER_TOOLCHAIN_PATH)/arm-none-eabi-objdump -s -j '.text#' \
		$(BUILD_DIR)/libco-arm/libco.o | \
		grep -q 'f06fa1e8 f0afb0e8 1eff2fe1'
endif

ifeq ($(strip $(BK3633_SDK_ROOT)),)
button_sdk_probe buzzer_sdk_probe battery_sdk_probe power_sdk_probe \
	time_sdk_probe:
	@echo "BK3633 SDK probe SKIP: BK3633_PATH is required"
else
button_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_SDK_ROOT)/src/driver/gpio \
		probes/src/button_probe.c

buzzer_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_SDK_ROOT)/src/driver/pwm \
		probes/src/buzzer_probe.c
endif

ifeq ($(strip $(BK3633_SDK_ROOT)),)
entropy_sdk_probe:
	@echo "BK3633 entropy SDK probe SKIP: BK3633_PATH is required"
else
entropy_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_SDK_ROOT)/src/header \
		probes/src/entropy_probe.c
	$(HOST_CC) $(HOST_CFLAGS) -DBK3633 -DCFG_CPU_BK3633 -fsyntax-only \
		-Iinclude -I$(REPO_ROOT)/libs/pal/include \
		-I$(BK3633_SDK_ROOT)/src/header \
		src/h2_bk3633_platform_entropy.c
endif

ifeq ($(strip $(BK3633_SDK_ROOT)),)
storage_sdk_probe:
	@echo "BK3633 storage SDK probe SKIP: BK3633_PATH is required"
else
storage_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_SDK_ROOT)/src/plf/nvds/api \
		-I$(BK3633_SDK_ROOT)/src/driver/flash \
		probes/src/probe_pal_storage_sdk.c

battery_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_SDK_ROOT)/src/driver/adc \
		-I$(BK3633_SDK_ROOT)/src/driver/gpio \
		probes/src/battery_probe.c

power_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_CONFIG_INCLUDE_DIR) \
		-I$(BK3633_SDK_ROOT)/src/header \
		-I$(BK3633_SDK_ROOT)/src/driver/icu \
		-I$(BK3633_SDK_ROOT)/src/driver/uart \
		-I$(BK3633_SDK_ROOT)/src/driver/uart2 \
		-I$(BK3633_SDK_ROOT)/src/ip/ble/hl/api \
		-I$(BK3633_SDK_ROOT)/src/ip/ble/hl/inc \
		-I$(BK3633_SDK_ROOT)/src/ip/ble/ll/api \
		-I$(BK3633_SDK_ROOT)/src/modules/common/api \
		-I$(BK3633_SDK_ROOT)/src/modules/ke/api \
		-I$(BK3633_SDK_ROOT)/src/plf/prf \
		-I$(BK3633_SDK_ROOT)/src/plf/rwip/api \
		-I$(BK3633_SDK_ROOT)/src/plf/refip/src/arch \
		-I$(BK3633_SDK_ROOT)/src/plf/refip/src/arch/compiler/armgcc \
		-I$(BK3633_SDK_ROOT)/src/plf/refip/src/arch/ll/armgcc \
		probes/src/power_probe.c

time_sdk_probe:
	$(HOST_CC) $(HOST_CFLAGS) -fsyntax-only \
		-I$(BK3633_CONFIG_INCLUDE_DIR) \
		-I$(BK3633_SDK_ROOT)/src/header \
		-I$(BK3633_SDK_ROOT)/src/driver/i2c \
		-I$(BK3633_SDK_ROOT)/src/driver/uart \
		-I$(BK3633_SDK_ROOT)/src/driver/uart2 \
		probes/src/time_probe.c
endif

$(BUILD_DIR)/mem_probe: \
		probe/mem_probe.c \
		src/h2_bk3633_platform_mem.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/log_probe: \
		probe/log_probe.c \
		src/h2_bk3633_platform_log.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/system_event_probe: \
		probe/system_event_probe.c \
		src/h2_bk3633_platform_system_event.c \
		$(REPO_ROOT)/libs/pal/src/unsupported/netif.c \
		$(REPO_ROOT)/libs/pal/src/unsupported/system_event.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) $(INCLUDES) $^ -o $@

$(BUILD_DIR)/firmware_info_probe: \
		probe/firmware_info_probe.c \
		src/h2_bk3633_platform_firmware_info.c
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) \
		-DH2_BK3633_FIRMWARE_VERSION=\"probe-version\" \
		$(INCLUDES) $^ -o $@

clean:
	rm -rf "$(BUILD_DIR)"
