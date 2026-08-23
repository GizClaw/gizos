# Adds the Bazel-owned native sources and archives to a repository-owned
# JieLi project. The selected layout defines the SDK source inventory,
# compiler flags, linker inputs, generated files and output paths before
# including this file; no SDK application Makefile is loaded here.
ifndef H2_BAZEL_COMPONENT_MANIFEST
$(error H2_BAZEL_COMPONENT_MANIFEST is required)
endif

include $(H2_BAZEL_COMPONENT_MANIFEST)

H2_BAZEL_OBJ_DIR := $(BUILD_DIR)/h2_bazel
H2_BAZEL_NATIVE_OBJS := $(foreach source,$(H2_BAZEL_NATIVE_SRCS),$(H2_BAZEL_OBJ_DIR)/$(subst /,_,$(source)).o)

define H2_BAZEL_COMPILE_RULE
$(H2_BAZEL_OBJ_DIR)/$(subst /,_,$(1)).o: $(1)
	$$(info +CC(h2) $$<)
	@$$(MKDIR) $$(@D)
	$$(QUITE) $$(CC) $$(CFLAGS) $$(DEFINES) $$(H2_BAZEL_DEFINES) $$(INCLUDES) $$(H2_BAZEL_NATIVE_INCLUDES) -c $$< -o $$@
endef
$(foreach source,$(H2_BAZEL_NATIVE_SRCS),$(eval $(call H2_BAZEL_COMPILE_RULE,$(source))))

H2_BAZEL_ARCHIVE_GROUP := $(if $(strip $(H2_BAZEL_ARCHIVES)),--start-group $(H2_BAZEL_ARCHIVES) --end-group,)

.PHONY: h2_link
h2_link: pre_build $(OBJS) $(H2_BAZEL_NATIVE_OBJS)
	$(info +LINK(h2) $(OUT_ELF))
	$(file >$(OBJ_FILE),$(OBJS) $(H2_BAZEL_NATIVE_OBJS))
	$(QUITE) $(LD) -o $(OUT_ELF) @$(OBJ_FILE) $(H2_BAZEL_ARCHIVE_GROUP) $(LFLAGS) $(LIBPATHS) $(LIBS)
