REPO_ROOT := $(abspath ../../..)
BUILD_DIR ?= build/probes
HOST_CC ?= cc
HOST_CFLAGS := -std=c11 -Wall -Wextra -Werror -Dthread_local=_Thread_local
INCLUDES := \
	-Iinclude \
	-Isrc \
	-Iprobe/stubs \
	-I$(REPO_ROOT)/libs/pal/include \
	-I$(REPO_ROOT)/libs/pal/providers/libco/include \
	-I$(REPO_ROOT)/libs/pal/providers/libco/src \
	-I$(REPO_ROOT)/third_party/libco
LIBCO_SOURCES := \
	$(REPO_ROOT)/libs/pal/providers/libco/src/h2_libco.c \
	$(REPO_ROOT)/libs/pal/providers/libco/src/h2_libco_pal_task.c \
	$(REPO_ROOT)/libs/pal/providers/libco/src/h2_libco_pal_time.c \
	$(REPO_ROOT)/libs/pal/providers/libco/src/h2_libco_pal_queue.c \
	$(REPO_ROOT)/libs/pal/providers/libco/src/h2_libco_pal_sync.c \
	$(REPO_ROOT)/third_party/libco/libco.c
PROBE_TARGETS := \
	$(BUILD_DIR)/runtime_state_probe \
	$(BUILD_DIR)/runtime_platform_failure_probe \
	$(BUILD_DIR)/runtime_start_failure_probe

.PHONY: probes clean

probes: $(PROBE_TARGETS)
	$(BUILD_DIR)/runtime_state_probe
	$(BUILD_DIR)/runtime_platform_failure_probe
	$(BUILD_DIR)/runtime_start_failure_probe
	@echo "BK3633 sdk_runtime probes OK"

$(PROBE_TARGETS): $(BUILD_DIR)/%: probe/%.c \
		probe/runtime_state_probe_stubs.c \
		src/h2_bk3633_sdk_runtime.c \
		$(LIBCO_SOURCES)
	@mkdir -p $(@D)
	$(HOST_CC) $(HOST_CFLAGS) -Wno-unused-value \
		-DBK3633_SDK_HAS_RWIP_GLUE=1 \
		$(INCLUDES) $^ -o $@

clean:
	rm -rf "$(BUILD_DIR)"
