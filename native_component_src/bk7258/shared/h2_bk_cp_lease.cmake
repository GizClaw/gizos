# The SDK exposes an address at OFFER time, before a client has a lease.
# Record only a DHCP ACK for which at least one lwIP send completed. The AP
# may use the private snapshot even if the best-effort customer hint is lost.
option(H2_BK_CP_LEASE_HOOK "Report accepted BK DHCP leases to the paired AP" ON)
if(H2_BK_CP_LEASE_HOOK)
  if(NOT H2_BK_CP_PSRAM_SERVICES)
    message(FATAL_ERROR "BK CP lease hook requires the paired DHCP service overlay")
  endif()
  set(buffer_source "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_dhcp_buffer.c")
  set(task_source "${CMAKE_CURRENT_BINARY_DIR}/h2_bk_cp_dhcp_task.c")
  file(READ "${buffer_source}" buffer_content)
  file(READ "${task_source}" task_content)

  set(response_start "static int send_response(int sock, struct sockaddr *addr, char *msg, int len)")
  set(response_end "#define ERROR_REFUSED 5")
  string(FIND "${buffer_content}" "${response_start}" begin)
  string(FIND "${buffer_content}" "${response_end}" end)
  if(begin EQUAL -1 OR end EQUAL -1 OR end LESS_EQUAL begin)
    message(FATAL_ERROR "Pinned BK DHCP send_response changed; revalidate ACK evidence")
  endif()
  math(EXPR length "${end} - ${begin}")
  string(SUBSTRING "${buffer_content}" ${begin} ${length} response)
  set(return_anchor "\treturn 0;\n}")
  string(FIND "${response}" "${return_anchor}" return_offset)
  if(return_offset EQUAL -1)
    message(FATAL_ERROR "Pinned BK DHCP successful send path changed")
  endif()
  set(accept_code "
    if (sock == dhcps.sock && len > 0 &&
        (size_t)len >= sizeof(struct bootp_header) + sizeof(struct bootp_option) + 1u) {
        const struct bootp_header *ack = (const struct bootp_header *)msg;
        const struct bootp_option *option =
            (const struct bootp_option *)(msg + sizeof(struct bootp_header));
        if (ack->op == BOOTP_OP_RESPONSE && option->type == BOOTP_OPTION_DHCP_MESSAGE &&
            option->length == 1u && *(const uint8_t *)option->value == DHCP_MESSAGE_ACK &&
            ack->yiaddr != 0u) {
            (void)h2_bk_wifi_lease_accept(ack->chaddr, lwip_ntohl(ack->yiaddr),
                                          lwip_ntohl(ack->xid));
        }
    }")
  string(REPLACE "${return_anchor}" "${accept_code}${return_anchor}"
    corrected_response "${response}")
  string(REPLACE "${response}" "${corrected_response}"
    corrected_buffer "${buffer_content}")
  file(WRITE "${buffer_source}"
    "#include \"h2_bk_wifi_lease.h\"\n${corrected_buffer}")

  set(start_anchor "ret = rtos_create_psram_thread(&dhcpd_thread,")
  string(FIND "${task_content}" "${start_anchor}" start_offset)
  if(start_offset EQUAL -1)
    message(FATAL_ERROR "Pinned BK DHCP server start changed; revalidate lease generation")
  endif()
  string(REPLACE "${start_anchor}"
    "h2_bk_wifi_lease_reset();\n\t${start_anchor}"
    corrected_task "${task_content}")
  file(WRITE "${task_source}"
    "#include \"h2_bk_wifi_lease.h\"\n${corrected_task}")

  armino_component_get_property(lwip_library lwip_intf_v2_1 COMPONENT_LIB)
  target_include_directories(${lwip_library} PRIVATE
    "${REPO_ROOT}/native_component_src/bk7258/shared")
endif()
