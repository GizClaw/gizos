# Only explicit public fixture inputs; never import operator environment values.
target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
if(DEFINED H2_E2E_WIFI_SSID OR DEFINED H2_E2E_WIFI_PASSWORD)
  if(NOT DEFINED H2_E2E_WIFI_SSID OR NOT DEFINED H2_E2E_WIFI_PASSWORD OR
     H2_E2E_WIFI_SSID STREQUAL "")
    message(FATAL_ERROR "Explicit fixture Wi-Fi requires SSID and password")
  endif()
  foreach(H2_E2E_WIFI_FIELD H2_E2E_WIFI_SSID H2_E2E_WIFI_PASSWORD)
    if("${${H2_E2E_WIFI_FIELD}}" MATCHES "[\r\n;]")
      message(FATAL_ERROR "${H2_E2E_WIFI_FIELD} contains an unsupported character")
    endif()
    string(REPLACE "\\" "\\\\" H2_E2E_WIFI_ESCAPED "${${H2_E2E_WIFI_FIELD}}")
    string(REPLACE "\"" "\\\"" H2_E2E_WIFI_ESCAPED "${H2_E2E_WIFI_ESCAPED}")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
      "${H2_E2E_WIFI_FIELD}=\"${H2_E2E_WIFI_ESCAPED}\"")
  endforeach()
endif()
