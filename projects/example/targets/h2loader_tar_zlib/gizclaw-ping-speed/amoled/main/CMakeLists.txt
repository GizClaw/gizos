if(NOT DEFINED H2_REPO_ROOT)
  get_filename_component(H2_REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../../../../../.." ABSOLUTE)
else()
  get_filename_component(H2_REPO_ROOT "${H2_REPO_ROOT}" ABSOLUTE)
endif()

function(h2_smoke_c_string out value)
  set(escaped "${value}")
  string(REPLACE "\\" "\\\\" escaped "${escaped}")
  string(REPLACE "\"" "\\\"" escaped "${escaped}")
  string(REPLACE "\r" "" escaped "${escaped}")
  string(REPLACE "\n" "" escaped "${escaped}")
  set(${out} "${escaped}" PARENT_SCOPE)
endfunction()

if(H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE)
  set(H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE_ABS "${H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE}")
  if(NOT IS_ABSOLUTE "${H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE_ABS}")
    get_filename_component(
      H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE_ABS
      "${H2_REPO_ROOT}/${H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE_ABS}"
      ABSOLUTE)
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE_ABS}")
  file(READ "${H2_GIZCLAW_CLIENT_PRIVATE_KEY_FILE_ABS}" H2_GIZCLAW_CLIENT_PRIVATE_KEY)
  string(STRIP "${H2_GIZCLAW_CLIENT_PRIVATE_KEY}" H2_GIZCLAW_CLIENT_PRIVATE_KEY)
  string(REGEX MATCH "(^|\n)[ \t]*private-key:[ \t]*([^\n\r]+)" H2_GIZCLAW_CLIENT_PRIVATE_KEY_MATCH "${H2_GIZCLAW_CLIENT_PRIVATE_KEY}")
  if(H2_GIZCLAW_CLIENT_PRIVATE_KEY_MATCH)
    set(H2_GIZCLAW_CLIENT_PRIVATE_KEY "${CMAKE_MATCH_2}")
    string(STRIP "${H2_GIZCLAW_CLIENT_PRIVATE_KEY}" H2_GIZCLAW_CLIENT_PRIVATE_KEY)
    string(REGEX REPLACE "^['\"]|['\"]$" "" H2_GIZCLAW_CLIENT_PRIVATE_KEY "${H2_GIZCLAW_CLIENT_PRIVATE_KEY}")
  endif()
endif()

if(H2_SMOKE_WIFI_ENV_FILE)
  set(H2_SMOKE_WIFI_ENV_FILE_ABS "${H2_SMOKE_WIFI_ENV_FILE}")
  if(NOT IS_ABSOLUTE "${H2_SMOKE_WIFI_ENV_FILE_ABS}")
    get_filename_component(H2_SMOKE_WIFI_ENV_FILE_ABS "${H2_REPO_ROOT}/${H2_SMOKE_WIFI_ENV_FILE_ABS}" ABSOLUTE)
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${H2_SMOKE_WIFI_ENV_FILE_ABS}")
  file(READ "${H2_SMOKE_WIFI_ENV_FILE_ABS}" H2_SMOKE_WIFI_ENV_TEXT)
  string(REGEX MATCH "(^|\n)[ \t]*ssid:[ \t]*([^\n\r]+)" H2_SMOKE_WIFI_SSID_MATCH "${H2_SMOKE_WIFI_ENV_TEXT}")
  if(H2_SMOKE_WIFI_SSID_MATCH)
    set(H2_SMOKE_WIFI_SSID "${CMAKE_MATCH_2}")
  endif()
  string(REGEX MATCH "(^|\n)[ \t]*password:[ \t]*([^\n\r]+)" H2_SMOKE_WIFI_PASSWORD_MATCH "${H2_SMOKE_WIFI_ENV_TEXT}")
  if(H2_SMOKE_WIFI_PASSWORD_MATCH)
    set(H2_SMOKE_WIFI_PASSWORD "${CMAKE_MATCH_2}")
  endif()
  string(STRIP "${H2_SMOKE_WIFI_SSID}" H2_SMOKE_WIFI_SSID)
  string(STRIP "${H2_SMOKE_WIFI_PASSWORD}" H2_SMOKE_WIFI_PASSWORD)
endif()

h2_smoke_c_string(H2_GIZCLAW_CLIENT_PRIVATE_KEY_C "${H2_GIZCLAW_CLIENT_PRIVATE_KEY}")
h2_smoke_c_string(H2_SMOKE_WIFI_SSID_C "${H2_SMOKE_WIFI_SSID}")
h2_smoke_c_string(H2_SMOKE_WIFI_PASSWORD_C "${H2_SMOKE_WIFI_PASSWORD}")

set(H2_SMOKE_GIZCLAW_GENERATED_C "${CMAKE_CURRENT_BINARY_DIR}/h2_smoke_gizclaw_secrets.c")
file(WRITE "${H2_SMOKE_GIZCLAW_GENERATED_C}"
"const char h2_gizclaw_client_private_key[] = \"${H2_GIZCLAW_CLIENT_PRIVATE_KEY_C}\";
const char h2_smoke_wifi_ssid[] = \"${H2_SMOKE_WIFI_SSID_C}\";
const char h2_smoke_wifi_password[] = \"${H2_SMOKE_WIFI_PASSWORD_C}\";
")

include("$ENV{H2_GIZOS_ROOT}/tools/bazel/native_component.cmake")
h2_bazel_native_component_sources(H2_MAIN_SOURCES main)
list(APPEND H2_MAIN_SOURCES "${H2_SMOKE_GIZCLAW_GENERATED_C}")

idf_component_register(
  SRCS ${H2_MAIN_SOURCES}
  INCLUDE_DIRS
    "${H2_REPO_ROOT}/projects/h2loader/libs/h2loader/include"
    "${H2_REPO_ROOT}/libs/pal/include"
    "${H2_REPO_ROOT}/libs/bundle/include"
    "${H2_REPO_ROOT}/libs/gizclaw/include"
    "${H2_REPO_ROOT}/projects/example/apps/gizclaw-ping-speed/app/include"
  REQUIRES
    app_update
    espcoredump
    esp_driver_usb_serial_jtag
    h2_firmware_lib
    h2_esp_board
    h2_gizclaw
    h2_h2loader_ble
    h2_h2loader_runtime
    h2_pal_core
    h2_firmware_lib
)

if(H2_GIZCLAW_SERVER_ENDPOINT)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE "H2_GIZCLAW_SERVER_ENDPOINT=\"${H2_GIZCLAW_SERVER_ENDPOINT}\"")
endif()
if(H2_GIZCLAW_CONNECT_TIMEOUT_MS)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE H2_GIZCLAW_CONNECT_TIMEOUT_MS=${H2_GIZCLAW_CONNECT_TIMEOUT_MS})
endif()

target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -Wall -Wextra -Werror)
