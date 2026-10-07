# lwIP's DHCP timer calls the status callback, then independently requests a
# radio disconnect. Preserve a current preferred IPv6 association at BOTH
# boundaries. A notification-only guard still disconnects IPv6-only after 20s.
# This replaces the pinned SDK source only in the invocation's build tree.
if(CONFIG_IPV6)
  armino_component_get_property(wifi_library bk_wifi COMPONENT_LIB)
  set(wifi_source "${H2_BK_CP_SDK_ROOT}/components/bk_wifi/src/wifi_netif.c")
  file(READ "${wifi_source}" corrected)
  string(REPLACE "\r\n" "\n" corrected "${corrected}")
  foreach(callback wifi_netif_notify_sta_dhcp_timeout wifi_netif_notify_sta_disconnect)
    set(anchor "void ${callback}(void)\n{")
    string(FIND "${corrected}" "${anchor}" offset)
    if(offset EQUAL -1)
      message(FATAL_ERROR "Pinned BK CP ${callback} signature changed")
    endif()
    string(REPLACE "${anchor}" "${anchor}\n    if (h2_bk_wifi_ipv6_ready()) return;"
      corrected "${corrected}")
  endforeach()
  set(output "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_wifi_netif.c")
  file(WRITE "${output}" "#include \"h2_bk_wifi_ipv6.h\"\n${corrected}")
  get_target_property(sources ${wifi_library} SOURCES)
  list(FILTER sources EXCLUDE REGEX "(^|/)wifi_netif\\.c$")
  list(APPEND sources "${output}")
  set_property(TARGET ${wifi_library} PROPERTY SOURCES ${sources})
  target_include_directories(${wifi_library} PRIVATE
    "${REPO_ROOT}/native_component_src/bk7258/shared")
endif()
