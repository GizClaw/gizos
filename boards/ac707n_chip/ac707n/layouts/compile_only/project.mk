# Repository-owned AC707N compile-only native project. The pinned SDK supplies
# reusable CPU, system, header, archive, linker and post-build inputs only.

H2_JIELI_LAYOUT_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

ifeq ($(OS), Windows_NT)
TOOL_DIR := C:/JL/pi32/bin
CC := clang.exe
CXX := clang.exe
LD := pi32v2-lto-wrapper.exe
AR := pi32v2-lto-ar.exe
MKDIR := mkdir_win -p
RM := rm -rf
SYS_LIB_DIR := C:/JL/pi32/pi32v2-lib/r3-large
SYS_INC_DIR := C:/JL/pi32/pi32v2-include
EXT_CFLAGS :=
FIXBAT := tools\utils\fixbat.exe
POST_SCRIPT := cpu\br35\tools\download.bat
RUN_POST_SCRIPT := $(POST_SCRIPT)
else
TOOL_DIR := /opt/jieli/pi32v2/bin
CC := clang
CXX := clang++
LD := lto-wrapper
AR := lto-ar
MKDIR := mkdir -p
RM := rm -rf
SYS_LIB_DIR := $(TOOL_DIR)/../lib/r3-large
SYS_INC_DIR := $(TOOL_DIR)/../include
EXT_CFLAGS := -D__SHELL__
FIXBAT := touch
POST_SCRIPT := cpu/br35/tools/download.sh
RUN_POST_SCRIPT := bash $(POST_SCRIPT)
endif

CC := $(TOOL_DIR)/$(CC)
CXX := $(TOOL_DIR)/$(CXX)
LD := $(TOOL_DIR)/$(LD)
AR := $(TOOL_DIR)/$(AR)
OUT_ELF := cpu/br35/tools/sdk.elf
OBJ_FILE := $(OUT_ELF).objs.txt
BUILD_DIR := objs

CFLAGS := \
	-target pi32v2 \
	-mcpu=r3 \
	-integrated-as \
	-flto \
	-Wuninitialized \
	-Wno-invalid-noreturn \
	-fno-common \
	-Oz \
	-mllvm -pi32v2-large-program=true \
	-fdiscrete-bitfield-abi \
	-g \
	-fallow-pointer-null \
	-fprefer-gnu-section \
	-Wno-shift-negative-value \
	-Wundef \
	-fms-extensions \

DEFINES := \
	-DSUPPORT_MS_EXTENSIONS \
	-DCONFIG_CPU_BR35 \
	-DCONFIG_RELEASE_ENABLE \
	-DCONFIG_UCOS_ENABLE \
	-DEVENT_HANDLER_NUM_CONFIG=2 \
	-DEVENT_TOUCH_ENABLE_CONFIG=0 \
	-DEVENT_POOL_SIZE_CONFIG=256 \
	-DCONFIG_EVENT_KEY_MAP_ENABLE=0 \
	-DTIMER_POOL_NUM_CONFIG=15 \
	-DAPP_ASYNC_POOL_NUM_CONFIG=0 \
	-DVFS_ENABLE=1 \
	-DUSE_SDFILE_NEW=1 \
	-DSDFILE_STORAGE=1 \
	-DVFS_FILE_POOL_NUM_CONFIG=1 \
	-DVM_MAX_SIZE_CONFIG=64*1024 \
	-DVM_ITEM_MAX_NUM=256 \
	-DCONFIG_UPDATA_ENABLE \
	-DCONFIG_OTA_UPDATA_ENABLE \
	-DCONFIG_ITEM_FORMAT_VM \
	-D__GCC_PI32V2__ \
	$(EXT_CFLAGS) \

INCLUDES := \
	-I$(H2_JIELI_LAYOUT_ROOT)/include \
	-Iinterface \
	-Iinterface/driver \
	-Iinterface/driver/device \
	-Iinterface/driver/cpu/br35 \
	-Iinterface/driver/cpu/periph \
	-Iinterface/utils \
	-Iinterface/utils/fs \
	-Iinterface/utils/device \
	-Iinterface/utils/generic \
	-Iinterface/media \
	-Iinterface/media/cpu/br35 \
	-Iinterface/system \
	-Iinterface/system/generic \
	-Iinterface/system/device \
	-Iinterface/system/fs \
	-Iinterface/update \
	-Iinterface/media/media_develop \
	-Icpu/br35 \
	-Iaudio/cpu/br35 \
	-I$(SYS_INC_DIR) \

c_SRC_FILES := \
	apps/common/debug/debug.c \
	cpu/power/msg.c \


S_SRC_FILES :=
s_SRC_FILES :=
cpp_SRC_FILES :=

LFLAGS := \
	--plugin-opt=-pi32v2-large-program=true \
	--plugin-opt=-pi32v2-always-use-itblock=false \
	--plugin-opt=-enable-ipra=true \
	--plugin-opt=-pi32v2-merge-max-offset=4096 \
	--plugin-opt=-pi32v2-enable-simd=true \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-global-merge-on-const \
	--plugin-opt=-inline-threshold=5 \
	--plugin-opt=-inline-max-allocated-size=32 \
	--plugin-opt=-inline-normal-into-special-section=true \
	--plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar \
	--plugin-opt=save-temps \
	--plugin-opt=-pi32v2-enable-rep-memop \
	--sort-common \
	--dont-complain-call-overflow \
	--plugin-opt=-used-symbol-file=cpu/br35/sdk_used_list.used \
	--gc-sections \
	--start-group \
	cpu/br35/liba/cpu.a \
	cpu/br35/liba/system.a \
	cpu/br35/liba/cfg_tool.a \
	cpu/br35/liba/vm.a \
	cpu/br35/liba/fs.a \
	cpu/br35/liba/device.a \
	cpu/br35/liba/printf.a \
	--end-group \
	-Tcpu/br35/sdk.ld \
	-M=cpu/br35/tools/sdk.map \
	--plugin-opt=mcpu=r3 \
	--plugin-opt=-mattr=+fprev1 \

LIBPATHS := -L$(SYS_LIB_DIR)
LIBS := \
	$(SYS_LIB_DIR)/libm.a \
	$(SYS_LIB_DIR)/libc.a \
	$(SYS_LIB_DIR)/libcompiler-rt.a \

c_OBJS := $(c_SRC_FILES:%.c=%.c.o)
S_OBJS := $(S_SRC_FILES:%.S=%.S.o)
s_OBJS := $(s_SRC_FILES:%.s=%.s.o)
cpp_OBJS := $(cpp_SRC_FILES:%.cpp=%.cpp.o)
OBJS := $(c_OBJS) $(S_OBJS) $(s_OBJS) $(cpp_OBJS)
DEP_FILES := $(OBJS:%.o=%.d)
OBJS := $(addprefix $(BUILD_DIR)/, $(OBJS))
DEP_FILES := $(addprefix $(BUILD_DIR)/, $(DEP_FILES))

VERBOSE ?= 0
ifeq ($(VERBOSE), 1)
QUITE :=
else
QUITE := @
endif

.PHONY: all clean pre_build
.SUFFIXES:

all: pre_build $(OUT_ELF)
	$(info +POST-BUILD)
	$(QUITE) $(RUN_POST_SCRIPT) sdk

pre_build:
	$(info +PRE-BUILD)
	$(QUITE) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P $(H2_JIELI_LAYOUT_ROOT)/include/sdk_used_list.c -o cpu/br35/sdk_used_list.used
	$(QUITE) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P cpu/br35/sdk_ld.c -o cpu/br35/sdk.ld
	$(QUITE) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -D__LD__ -E -P cpu/br35/tools/isd_config_rule.c -o cpu/br35/tools/isd_config.ini

clean:
	$(QUITE) $(RM) $(OUT_ELF)
	$(QUITE) $(RM) $(BUILD_DIR)

$(BUILD_DIR)/%.c.o: %.c
	$(info +CC $<)
	$(QUITE) $(MKDIR) $(@D)
	$(QUITE) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/%.S.o: %.S
	$(info +AS $<)
	$(QUITE) $(MKDIR) $(@D)
	$(QUITE) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/%.s.o: %.s
	$(info +AS $<)
	$(QUITE) $(MKDIR) $(@D)
	$(QUITE) $(CC) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MF $(@:.o=.d) -c $< -o $@

$(BUILD_DIR)/%.cpp.o: %.cpp
	$(info +CXX $<)
	$(QUITE) $(MKDIR) $(@D)
	$(QUITE) $(CXX) $(CXXFLAGS) $(CFLAGS) $(DEFINES) $(INCLUDES) -MMD -MF $(@:.o=.d) -c $< -o $@

-include $(DEP_FILES)

include $(H2_JIELI_PROJECT_RULES)
