# Controlled BK3633 GCC source manifest. This build description is separate
# from sdk_runtime lifecycle code and does not select a board or image.
#
# The allroles BLE stack is supplied by the prebuilt stack ELF selected by the
# board flavor. The RWIP sources below are the small SDK-native host/kernel
# layer used by sdk_runtime. Do not add controller/LL/profile sources unless
# the stack asset contract is deliberately changed.

BK3633_SDK_STARTUP_SOURCES := \
  $(BK3633_SDK_ROOT)/src/system/startup_boot.S

BK3633_SDK_DRIVER_SOURCES := \
  $(BK3633_SDK_ROOT)/src/driver/adc/adc.c \
  $(BK3633_SDK_ROOT)/src/driver/uart/uart.c \
  $(BK3633_SDK_ROOT)/src/driver/uart2/uart2.c \
  $(BK3633_SDK_ROOT)/src/driver/wdt/wdt.c \
  $(BK3633_SDK_ROOT)/src/driver/timer0/timer0.c \
  $(BK3633_SDK_ROOT)/src/driver/gpio/gpio.c \
  $(BK3633_SDK_ROOT)/src/driver/pwm/pwm.c \
  $(BK3633_SDK_ROOT)/src/driver/icu/icu.c \
  $(BK3633_SDK_ROOT)/src/driver/intc/intc.c \
  $(BK3633_SDK_ROOT)/src/driver/i2c/i2c.c \
  $(BK3633_SDK_ROOT)/src/driver/rf/rf_xvr.c

BK3633_SDK_RWIP_SOURCES := \
  $(BK3633_SDK_ROOT)/src/plf/rwip/src/rwbt.c \
  $(BK3633_SDK_ROOT)/src/plf/rwip/src/rwble.c \
  $(BK3633_SDK_ROOT)/src/plf/rwip/src/rwip.c

# Storage providers consume these existing SDK primitives. Keep the grouping
# explicit so image manifests cannot silently omit persistent storage support.
BK3633_SDK_STORAGE_SOURCES := \
  $(BK3633_SDK_ROOT)/src/driver/flash/flash.c \
  $(BK3633_SDK_ROOT)/src/plf/nvds/src/nvds.c

# Profile registration remains project-owned.  Do not add prf.c here until a
# project supplies its complete profile source/config set.
BK3633_SDK_PROFILE_SOURCES :=

# The pinned SDK header declares attmdb_svc_visibility_set(), but its source
# closure omits the implementation. Keep the compatibility object separate
# from SDK-native sources so repository stack-usage limits still apply.
BK3633_SDK_COMPAT_SOURCES := \
  $(REPO_ROOT)/native_component_src/bk3633/sdk_build/src/h2_bk3633_attmdb_compat.c

BK3633_SDK_SOURCES := \
  $(BK3633_SDK_STARTUP_SOURCES) \
  $(BK3633_SDK_DRIVER_SOURCES) \
  $(BK3633_SDK_RWIP_SOURCES) \
  $(BK3633_SDK_STORAGE_SOURCES)
