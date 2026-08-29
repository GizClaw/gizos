set(REPO_ROOT "$ENV{H2_GIZOS_ROOT}")
if(POLICY CMP0079)
  cmake_policy(SET CMP0079 NEW)
endif()
set(BK_ARMINO_PATH "$ENV{ARMINO_PATH}")
set(BK_FLASHDB_DIR "${BK_ARMINO_PATH}/components/flashdb")
set(BK_WIFI_DRIVER_DIR "${BK_ARMINO_PATH}/components/bk_wifi_driver")
option(H2_BK_PLATFORM_ENABLE_WEBRTC "Enable BK WebRTC platform implementation backed by H2Peer" OFF)
option(H2_BK_PLATFORM_BT_ALLOCATOR_PSRAM "Route BK Bluetooth host allocations to PSRAM" OFF)
set(H2_BK_PLATFORM_CORE_PRIV_REQUIRES
  bk_common
  bk_bluetooth
  easy_flash
  bk_netif
  bk_rtos
  driver
  h2_firmware_lib
  lwip_intf_v2_1
  psa_mbedtls)
set(H2_BK_PLATFORM_CORE_PRIV_INCLUDE_DIRS
  "${CMAKE_CURRENT_LIST_DIR}/src/flashdb"
  "${BK_FLASHDB_DIR}/inc"
  "${BK_FLASHDB_DIR}/port"
  "${BK_FLASHDB_DIR}/port/fal/inc")
set(H2_BK_PLATFORM_CORE_COMPILE_DEFINITIONS
  H2_BK_PLATFORM_CORE_ENABLE_MQTT=0
  H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH=0)

if(H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH)
  if(NOT EXISTS "${BK_WIFI_DRIVER_DIR}/wdrv_cntrl.h")
    set(BK_WIFI_DRIVER_DIR "${BK_ARMINO_PATH}/ap/components/bk_wifi_driver")
  endif()
  if(NOT EXISTS "${BK_WIFI_DRIVER_DIR}/wdrv_cntrl.h")
    message(FATAL_ERROR "BK Wi-Fi driver control header not found: ${BK_WIFI_DRIVER_DIR}")
  endif()
  list(REMOVE_ITEM H2_BK_PLATFORM_CORE_COMPILE_DEFINITIONS
    H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH=0)
  list(APPEND H2_BK_PLATFORM_CORE_COMPILE_DEFINITIONS
    H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH=1)
  list(APPEND H2_BK_PLATFORM_CORE_PRIV_INCLUDE_DIRS
    "${BK_WIFI_DRIVER_DIR}")
  set(H2_BK_PLATFORM_WIFI_EVENT_DISPATCH_SOURCE
    "src/h2_bk_platform_wifi_event_dispatch.c")
endif()

set(H2_BK_FIRMWARE_VERSION_FILE "$ENV{H2_FIRMWARE_VERSION_FILE}")
if(NOT H2_BK_FIRMWARE_VERSION_FILE STREQUAL "")
  if(NOT EXISTS "${H2_BK_FIRMWARE_VERSION_FILE}")
    message(FATAL_ERROR "H2_FIRMWARE_VERSION_FILE does not exist: ${H2_BK_FIRMWARE_VERSION_FILE}")
  endif()
  set_property(
    DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${H2_BK_FIRMWARE_VERSION_FILE}")
  file(
    STRINGS "${H2_BK_FIRMWARE_VERSION_FILE}" H2_BK_FIRMWARE_VERSION
    LIMIT_COUNT 1)
else()
  set(H2_BK_FIRMWARE_VERSION "$ENV{H2_FIRMWARE_VERSION}")
endif()
if(H2_BK_FIRMWARE_VERSION STREQUAL "")
  execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY "${REPO_ROOT}"
    OUTPUT_VARIABLE H2_BK_FIRMWARE_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE H2_BK_FIRMWARE_VERSION_RESULT)
  if(NOT H2_BK_FIRMWARE_VERSION_RESULT EQUAL 0 OR H2_BK_FIRMWARE_VERSION STREQUAL "")
    message(FATAL_ERROR "failed to derive BK firmware version")
  endif()
endif()

if(H2_BK_PLATFORM_CORE_ENABLE_MQTT)
  list(APPEND H2_BK_PLATFORM_CORE_PRIV_REQUIRES
    h2_firmware_lib)
  list(APPEND H2_BK_PLATFORM_CORE_PRIV_INCLUDE_DIRS
    "${REPO_ROOT}/libs/pal/providers/coremqtt/include")
  list(REMOVE_ITEM H2_BK_PLATFORM_CORE_COMPILE_DEFINITIONS
    H2_BK_PLATFORM_CORE_ENABLE_MQTT=0)
  list(APPEND H2_BK_PLATFORM_CORE_COMPILE_DEFINITIONS
    H2_BK_PLATFORM_CORE_ENABLE_MQTT=1)
endif()

set(H2_BK_WEBRTC_SOURCE "src/h2_bk_platform_webrtc_unsupported.c")
if(H2_BK_PLATFORM_ENABLE_WEBRTC)
  set(H2_BK_WEBRTC_SOURCE "src/h2_bk_platform_webrtc.c")
endif()

set(H2_BK_PLATFORM_CORE_SRCS
  "src/h2_bk_platform_atomic.c"
  "src/h2_bk_platform_mem.c"
  "src/h2_bk_platform_log.c"
  "src/h2_bk_platform_firmware_info.c"
  "src/h2_bk_platform_sync.c"
  "src/h2_bk_platform_net.c"
  "src/h2_bk_platform_netif.c"
  "src/h2_bk_platform_task.c"
  "src/h2_bk_platform_queue.c"
  "src/h2_bk_platform_time.c"
  "src/h2_bk_uart_control_tracker.c"
  "src/h2_bk_platform_uart_io_stream.c"
  "src/h2_bk_platform_crypto.c"
  "src/h2_bk_platform_dtls.c"
  "src/h2_bk_platform_pref_flashdb.c"
  "src/h2_bk_platform_pref_flashdb_port.c"
  "src/h2_bk_platform_mqtt.c"
  "${H2_BK_WEBRTC_SOURCE}"
  "src/h2_bk_platform_system_event.c"
  "src/h2_bk_platform_modem_unsupported.c"
  "src/h2_bk_platform_wifi.c"
  "src/h2_bk_platform_wifi_csi.c"
  ${H2_BK_PLATFORM_WIFI_EVENT_DISPATCH_SOURCE}
  "src/h2_bk_platform_wifi_settings.c"
  "src/h2_bk_platform_ble.c"
  "src/h2_bk_platform_ble_heap.c")
include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_select_sources(
  H2_COMPONENT_SOURCES
  h2_pal_core
  ap
  ${H2_BK_PLATFORM_CORE_SRCS})

armino_component_register(
  SRCS ${H2_COMPONENT_SOURCES}
    "${BK_FLASHDB_DIR}/src/fdb.c"
    "${BK_FLASHDB_DIR}/src/fdb_kvdb.c"
    "${BK_FLASHDB_DIR}/src/fdb_utils.c"
    "${BK_FLASHDB_DIR}/port/fal/src/fal.c"
    "${BK_FLASHDB_DIR}/port/fal/src/fal_flash.c"
    "${BK_FLASHDB_DIR}/port/fal/src/fal_partition.c"
  INCLUDE_DIRS
    "include"
    "${REPO_ROOT}/libs/pal/include"
    "${REPO_ROOT}/libs/pal/providers/corehttp/include"
    "${REPO_ROOT}/libs/pal/providers/h2peer/include"
    "${REPO_ROOT}/libs/pal/providers/h2sctp/include"
  PRIV_INCLUDE_DIRS
    ${H2_BK_PLATFORM_CORE_PRIV_INCLUDE_DIRS}
  PRIV_REQUIRES
    ${H2_BK_PLATFORM_CORE_PRIV_REQUIRES})

target_compile_definitions(${COMPONENT_LIB} PRIVATE
  ${H2_BK_PLATFORM_CORE_COMPILE_DEFINITIONS}
  H2_BK_FIRMWARE_VERSION="${H2_BK_FIRMWARE_VERSION}"
  MBEDTLS_CHACHA20_C
  MBEDTLS_POLY1305_C
  MBEDTLS_CHACHAPOLY_C
  MBEDTLS_SSL_DTLS_SRTP
  MBEDTLS_SSL_SRV_C
  MBEDTLS_X509_CREATE_C
  MBEDTLS_X509_CRT_WRITE_C)

# The SDK reduced Mbed TLS profile omits ALPN even though the HTTP transport
# requires an explicit http/1.1 offer. Enable the existing public Mbed TLS
# implementation for both the SDK library and its consumers.
target_compile_definitions(__armino_psa_mbedtls PUBLIC MBEDTLS_SSL_ALPN)

if(H2_BK_PLATFORM_ENABLE_WEBRTC)
  # Keep the SDK's wrapped libc entry points and its mismatched timer ABI out
  # of LTO. They remain ordinary archive symbols that the final plugin link
  # can resolve without changing the existing platform behavior.
  set_property(SOURCE "${BK_ARMINO_PATH}/components/bk_rtos/platform_stub.c"
    TARGET_DIRECTORY __armino_bk_rtos
    APPEND PROPERTY COMPILE_OPTIONS "-fno-lto")
  set_property(SOURCE "${BK_ARMINO_PATH}/middleware/driver/uart/printf.c"
    TARGET_DIRECTORY __armino_driver
    APPEND PROPERTY COMPILE_OPTIONS "-fno-lto")
  set_property(SOURCE
    "${BK_ARMINO_PATH}/middleware/soc/bk7258_ap/hal/timer_hal.c"
    "${BK_ARMINO_PATH}/middleware/soc/bk7258_ap/hal/sys_hal.c"
    TARGET_DIRECTORY __armino_bk7258_ap
    APPEND PROPERTY COMPILE_OPTIONS "-fno-lto")
  set_property(SOURCE "src/h2_bk_platform_atomic.c"
    TARGET_DIRECTORY ${COMPONENT_LIB}
    APPEND PROPERTY COMPILE_OPTIONS "-fno-lto")
  # Armino appends -fno-lto while constructing the build. This component is
  # registered afterward, so its link option is the image's final policy.
  set(H2_BK_WEBRTC_LINK_OPTIONS
    "-Os"
    "-flto"
    "-Wl,-u,__wrap_printf"
    "-Wl,-u,__wrap_vsnprintf"
    "-Wl,-u,__wrap_calloc"
    "-Wl,-u,__atomic_load_8"
    "-Wl,-u,__atomic_store_8")
  armino_build_set_property(
    LINK_OPTIONS "${H2_BK_WEBRTC_LINK_OPTIONS}" APPEND)
endif()

armino_component_get_property(
  H2_BK_MBEDTLS_COMPONENT_LIB psa_mbedtls COMPONENT_LIB)
target_compile_definitions(
  ${H2_BK_MBEDTLS_COMPONENT_LIB} PRIVATE
    MBEDTLS_SSL_DTLS_SRTP
    MBEDTLS_SSL_SRV_C
    MBEDTLS_X509_CREATE_C
    MBEDTLS_X509_CRT_WRITE_C)

# The BK v3.1.1 minimal configuration registers PSA source files but leaves
# MBEDTLS_PSA_CRYPTO_C disabled, producing an empty public PSA implementation.
# Enable only the standardized Crypto PAL prerequisites on the SDK-owned
# component so every BK launcher receives one complete provider without
# copying SDK crypto sources into Firmwares.
armino_component_get_property(
  H2_BK_PSA_MBEDTLS_LIB psa_mbedtls COMPONENT_LIB)
target_compile_definitions(${H2_BK_PSA_MBEDTLS_LIB} PRIVATE
  MBEDTLS_PSA_CRYPTO_C
  MBEDTLS_ECP_DP_CURVE25519_ENABLED
  MBEDTLS_CHACHA20_C
  MBEDTLS_POLY1305_C
  MBEDTLS_CHACHAPOLY_C)

if(CONFIG_BT AND H2_BK_PLATFORM_BT_ALLOCATOR_PSRAM)
  target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=_malloc_wrapper"
    "-Wl,--wrap=_free_wrapper"
    "-Wl,-u,__wrap__malloc_wrapper"
    "-Wl,-u,__wrap__free_wrapper")
endif()

if(H2_BK_PLATFORM_WIFI_CSI_SAFE_DISPATCH)
  target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=wdrv_rx_handle_event")
endif()
