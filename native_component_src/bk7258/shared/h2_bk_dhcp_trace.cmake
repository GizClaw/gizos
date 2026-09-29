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
  # The AP DHCP server uses CP lwIP's low_level_output. Keep the original
  # pre-send event and add its actual radio-queue sender return using a local
  # copy of metadata, since bmsg_tx_sender may consume the pbuf.
  set(H2_BK_TRACE_LOCAL_SEND_ANCHOR "ret = bmsg_tx_sender(p, (uint32_t)vif_idx);")
  string(FIND "${H2_BK_TRACE_CONTENT}"
    "${H2_BK_TRACE_LOCAL_SEND_ANCHOR}" H2_BK_TRACE_LOCAL_SEND_OFFSET)
  if(H2_BK_TRACE_LOCAL_SEND_OFFSET EQUAL -1)
    message(FATAL_ERROR "Pinned CP local TX sender changed; revalidate DHCP diagnostics")
  endif()
  string(REPLACE "${H2_BK_TRACE_LOCAL_SEND_ANCHOR}"
    "h2_bk_dhcp_entry_t h2_trace_result;
        int h2_trace_captured = h2_bk_dhcp_capture(5u, p, netif, vif_idx,
            netif->state ? wifi_netif_vif_to_netif_type(netif->state) : -1,
            &h2_trace_result);
        ${H2_BK_TRACE_LOCAL_SEND_ANCHOR}
        if (h2_trace_captured) {
            h2_trace_result.send_rc = (uint32_t)ret;
            h2_bk_dhcp_record(&h2_trace_result);
        }"
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

  # Host/AP frames bypass CP's lwIP output. Interpose only the three real
  # controller sender call sites in a guarded build-tree copy; capture before
  # ownership transfer, then record the sender's return using saved metadata.
  set(H2_BK_TRACE_DP_SOURCE "${H2_BK_TRACE_SDK_ROOT}/components/controller_if/cif_wifi_dp.c")
  file(READ "${H2_BK_TRACE_DP_SOURCE}" H2_BK_TRACE_DP_CONTENT)
  string(REPLACE "\r\n" "\n" H2_BK_TRACE_DP_CONTENT "${H2_BK_TRACE_DP_CONTENT}")
  string(FIND "${H2_BK_TRACE_DP_CONTENT}"
    "extern int bmsg_tx_sender(struct pbuf *p, uint32_t vif_idx);"
    H2_BK_TRACE_DP_ANCHOR)
  string(REGEX MATCHALL "bmsg_tx_sender\\(" H2_BK_TRACE_DP_CALLS "${H2_BK_TRACE_DP_CONTENT}")
  list(LENGTH H2_BK_TRACE_DP_CALLS H2_BK_TRACE_DP_COUNT)
  if(H2_BK_TRACE_DP_ANCHOR EQUAL -1 OR NOT H2_BK_TRACE_DP_COUNT EQUAL 4)
    message(FATAL_ERROR "Pinned CP host TX sender changed; revalidate DHCP diagnostics")
  endif()
  set(H2_BK_TRACE_DP_CORRECTED "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_dhcp_wifi_dp.c")
  set(H2_BK_TRACE_DP_PREFIX "#include \"h2_bk_dhcp_trace.h\"
#include \"bk_private/bk_wifi.h\"
extern int bmsg_tx_sender(struct pbuf *p, uint32_t vif_idx);
static int h2_bk_dhcp_bmsg_tx_sender(struct pbuf *p, uint32_t vif_idx) {
    int id = (int)vif_idx - 0xF;
    void *vif = wifi_netif_vifid_to_vif(id);
    struct netif *netif = vif ? (struct netif *)wifi_netif_get_vif_private_data(vif) : NULL;
    h2_bk_dhcp_entry_t entry;
    int captured = h2_bk_dhcp_capture(3u, p, netif, id,
        vif ? wifi_netif_vif_to_netif_type(vif) : -1, &entry);
    if (captured) h2_bk_dhcp_record(&entry);
    int rc = bmsg_tx_sender(p, vif_idx);
    if (captured) {
        entry.dir = 4u;
        entry.send_rc = (uint32_t)rc;
        h2_bk_dhcp_record(&entry);
    }
    return rc;
}
#define bmsg_tx_sender h2_bk_dhcp_bmsg_tx_sender
")
  file(WRITE "${H2_BK_TRACE_DP_CORRECTED}"
    "${H2_BK_TRACE_DP_PREFIX}${H2_BK_TRACE_DP_CONTENT}")
  get_target_property(H2_BK_TRACE_DP_SOURCES ${H2_BK_CP_CONTROLLER_LIB} SOURCES)
  list(FILTER H2_BK_TRACE_DP_SOURCES EXCLUDE REGEX "(^|/)cif_wifi_dp\\.c$")
  list(APPEND H2_BK_TRACE_DP_SOURCES "${H2_BK_TRACE_DP_CORRECTED}")
  set_property(TARGET ${H2_BK_CP_CONTROLLER_LIB} PROPERTY SOURCES ${H2_BK_TRACE_DP_SOURCES})
endif()
