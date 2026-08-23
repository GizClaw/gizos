# Wraps a JieLi SDK application Makefile so the Bazel-owned native sources
# and archives enter the SDK link without editing the pinned SDK.
#
# The runner invokes it from the SDK project directory:
#   make -f <this file> H2_BAZEL_COMPONENT_MANIFEST=<manifest.mk> TOOL_DIR=... h2_link
#
# The SDK Makefile (included first) owns CFLAGS/DEFINES/INCLUDES, the SDK
# object list, linker flags and the ELF path; this file only adds the
# Firmwares objects/archives to the final link and never runs the SDK's
# host-client cloud post-build.
ifndef H2_BAZEL_COMPONENT_MANIFEST
$(error H2_BAZEL_COMPONENT_MANIFEST is required)
endif

include Makefile
include $(H2_BAZEL_COMPONENT_MANIFEST)

H2_BAZEL_OBJ_DIR := $(BUILD_DIR)/h2_bazel
H2_BAZEL_NATIVE_OBJS := $(foreach source,$(H2_BAZEL_NATIVE_SRCS),$(H2_BAZEL_OBJ_DIR)/$(subst /,_,$(source)).o)

# Each Firmwares source compiles with the SDK's own flags plus the Bazel
# include roots and defines; the object name mangles the full path so sources
# with equal basenames never collide.
define H2_BAZEL_COMPILE_RULE
$(H2_BAZEL_OBJ_DIR)/$(subst /,_,$(1)).o: $(1)
	$$(info +CC(h2) $$<)
	@$$(MKDIR) $$(@D)
	$$(QUITE) $$(CC) $$(CFLAGS) $$(DEFINES) $$(H2_BAZEL_DEFINES) $$(INCLUDES) $$(H2_BAZEL_NATIVE_INCLUDES) -c $$< -o $$@
endef
$(foreach source,$(H2_BAZEL_NATIVE_SRCS),$(eval $(call H2_BAZEL_COMPILE_RULE,$(source))))

# The SDK application entry keeps compiling (its globals are referenced from
# other SDK files) but its app_main symbol is renamed so the Firmwares
# launcher provides the real one.
ifneq ($(strip $(H2_BAZEL_SDK_ENTRY_SOURCE)),)
$(BUILD_DIR)/$(H2_BAZEL_SDK_ENTRY_SOURCE).o: DEFINES += -Dapp_main=h2_jieli_sdk_app_main_replaced
endif

H2_BAZEL_SDK_OBJS := $(filter-out $(addprefix $(BUILD_DIR)/,$(H2_BAZEL_EXCLUDE_OBJS)),$(OBJS))
H2_BAZEL_ARCHIVE_GROUP := $(if $(strip $(H2_BAZEL_ARCHIVES)),--start-group $(H2_BAZEL_ARCHIVES) --end-group,)

.PHONY: h2_link
h2_link: pre_build $(H2_BAZEL_SDK_OBJS) $(H2_BAZEL_NATIVE_OBJS)
	$(info +LINK(h2) $(OUT_ELF))
	$(file >$(OBJ_FILE),$(H2_BAZEL_SDK_OBJS) $(H2_BAZEL_NATIVE_OBJS))
	$(QUITE) $(LD) -o $(OUT_ELF) @$(OBJ_FILE) $(H2_BAZEL_ARCHIVE_GROUP) $(LFLAGS) $(LIBPATHS) $(LIBS)
