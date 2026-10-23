# AP owns WDRV through VNET; WIFI_ENABLE belongs to the CP radio stack.
# Match the SDK driver's own selection, including AP builds without the radio.
if(CONFIG_WIFI_VNET_CONTROLLER)
  armino_component_get_property(H2_BK_WDRV_LIB bk_wifi_driver COMPONENT_LIB)
  set(H2_BK_WDRV_INPUT "${BK_ARMINO_PATH}/components/bk_wifi_driver/wdrv_tx.c")
  set(H2_BK_WDRV_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_sdk_wdrv_tx.c")
  include("${CMAKE_CURRENT_LIST_DIR}/h2_bk_wdrv_confirm.cmake")
  get_target_property(H2_BK_WDRV_SOURCES ${H2_BK_WDRV_LIB} SOURCES)
  list(FILTER H2_BK_WDRV_SOURCES EXCLUDE REGEX "(^|/)wdrv_tx\\.c$")
  list(APPEND H2_BK_WDRV_SOURCES "${H2_BK_WDRV_OUTPUT}")
  set_property(TARGET ${H2_BK_WDRV_LIB} PROPERTY SOURCES ${H2_BK_WDRV_SOURCES})
  target_include_directories(${H2_BK_WDRV_LIB} PRIVATE
    "${BK_ARMINO_PATH}/components/bk_wifi_driver")
endif()
