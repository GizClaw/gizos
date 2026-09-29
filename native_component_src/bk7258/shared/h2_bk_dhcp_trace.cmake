# Read-only packet diagnostics, disabled for production unless explicitly opted in.
option(H2_BK_DHCP_TRACE "Trace bounded DHCP metadata on CP's actual packet path" OFF)
if(H2_BK_DHCP_TRACE)
  set(H2_BK_TRACE_SDK_ROOT "$ENV{ARMINO_PATH}")
  set(H2_BK_TRACE_SOURCE "${H2_BK_TRACE_SDK_ROOT}/components/lwip_intf_v2_1/lwip-2.1.2/port/wlanif.c")
  file(READ "${H2_BK_TRACE_SOURCE}" H2_BK_TRACE_CONTENT)
  string(REPLACE "\r\n" "\n" H2_BK_TRACE_CONTENT "${H2_BK_TRACE_CONTENT}")
  set(H2_BK_TRACE_TX_ANCHOR "uint8_t vif_idx = wifi_netif_vif_to_vifid(netif->state);")
  set(H2_BK_TRACE_RX_ANCHOR "netif = vif ? (struct netif *)wifi_netif_get_vif_private_data(vif) : NULL;")
  foreach(H2_BK_TRACE_ANCHOR IN ITEMS H2_BK_TRACE_TX_ANCHOR H2_BK_TRACE_RX_ANCHOR)
    string(FIND "${H2_BK_TRACE_CONTENT}" "${${H2_BK_TRACE_ANCHOR}}" H2_BK_TRACE_OFFSET)
    if(H2_BK_TRACE_OFFSET EQUAL -1)
      message(FATAL_ERROR "Pinned CP packet-path anchor changed; revalidate DHCP diagnostics")
    endif()
  endforeach()
  string(REPLACE "${H2_BK_TRACE_TX_ANCHOR}"
    "${H2_BK_TRACE_TX_ANCHOR}\n    h2_bk_dhcp_trace(\"TX\", p, netif, vif_idx, netif->state ? wifi_netif_vif_to_netif_type(netif->state) : -1);"
    H2_BK_TRACE_CONTENT "${H2_BK_TRACE_CONTENT}")
  string(REPLACE "${H2_BK_TRACE_RX_ANCHOR}"
    "${H2_BK_TRACE_RX_ANCHOR}\n    h2_bk_dhcp_trace(\"RX\", p, netif, iface, vif ? wifi_netif_vif_to_netif_type(vif) : -1);"
    H2_BK_TRACE_CONTENT "${H2_BK_TRACE_CONTENT}")
  set(H2_BK_TRACE_CORRECTED "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_dhcp_wlanif.c")
  file(WRITE "${H2_BK_TRACE_CORRECTED}" "#include \"h2_bk_dhcp_trace.h\"\n${H2_BK_TRACE_CONTENT}")
  armino_component_get_property(H2_BK_TRACE_LIB lwip_intf_v2_1 COMPONENT_LIB)
  get_target_property(H2_BK_TRACE_SOURCES ${H2_BK_TRACE_LIB} SOURCES)
  list(FILTER H2_BK_TRACE_SOURCES EXCLUDE REGEX "(^|/)wlanif\\.c$")
  list(APPEND H2_BK_TRACE_SOURCES "${H2_BK_TRACE_CORRECTED}")
  set_property(TARGET ${H2_BK_TRACE_LIB} PROPERTY SOURCES ${H2_BK_TRACE_SOURCES})
  target_include_directories(${H2_BK_TRACE_LIB} PRIVATE
    "${REPO_ROOT}/native_component_src/bk7258/shared")

  # Host/AP frames bypass CP's lwIP output and enter the real radio TX queue
  # through controller_if. Record after ownership/free-return checks, before
  # cloning or submitting that pbuf. This does not change packet ownership.
  set(H2_BK_TRACE_DP_SOURCE "${H2_BK_TRACE_SDK_ROOT}/components/controller_if/cif_wifi_dp.c")
  file(READ "${H2_BK_TRACE_DP_SOURCE}" H2_BK_TRACE_DP_CONTENT)
  string(REPLACE "\r\n" "\n" H2_BK_TRACE_DP_CONTENT "${H2_BK_TRACE_DP_CONTENT}")
  set(H2_BK_TRACE_DP_ANCHOR "    CIF_STATS_INC(buf_in_txdata);")
  string(FIND "${H2_BK_TRACE_DP_CONTENT}" "${H2_BK_TRACE_DP_ANCHOR}" H2_BK_TRACE_DP_OFFSET)
  if(H2_BK_TRACE_DP_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned CP host TX anchor changed; revalidate DHCP diagnostics")
  endif()
  string(REPLACE "${H2_BK_TRACE_DP_ANCHOR}"
    "${H2_BK_TRACE_DP_ANCHOR}
    {
        int h2_trace_vif_id = (int)vif_id - 0xF;
        void *h2_trace_vif = wifi_netif_vifid_to_vif(h2_trace_vif_id);
        struct netif *h2_trace_netif = h2_trace_vif
            ? (struct netif *)wifi_netif_get_vif_private_data(h2_trace_vif) : NULL;
        h2_bk_dhcp_trace(\"TX\", pbuf, h2_trace_netif, h2_trace_vif_id,
            h2_trace_vif ? wifi_netif_vif_to_netif_type(h2_trace_vif) : -1);
    }"
    H2_BK_TRACE_DP_CONTENT "${H2_BK_TRACE_DP_CONTENT}")
  set(H2_BK_TRACE_DP_CORRECTED "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_dhcp_wifi_dp.c")
  file(WRITE "${H2_BK_TRACE_DP_CORRECTED}"
    "#include \"h2_bk_dhcp_trace.h\"\n${H2_BK_TRACE_DP_CONTENT}")
  get_target_property(H2_BK_TRACE_DP_SOURCES ${H2_BK_CP_CONTROLLER_LIB} SOURCES)
  list(FILTER H2_BK_TRACE_DP_SOURCES EXCLUDE REGEX "(^|/)cif_wifi_dp\\.c$")
  list(APPEND H2_BK_TRACE_DP_SOURCES "${H2_BK_TRACE_DP_CORRECTED}")
  set_property(TARGET ${H2_BK_CP_CONTROLLER_LIB} PROPERTY SOURCES ${H2_BK_TRACE_DP_SOURCES})
endif()
