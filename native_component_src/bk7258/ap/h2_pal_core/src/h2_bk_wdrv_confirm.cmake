# Patch only the invocation-local SDK copy. RX traversal and timeout removal
# already own cfm_lock; TX's header-local spinlock cannot exclude another TU.
file(READ "${H2_BK_WDRV_INPUT}" h2_wdrv_source)
string(REPLACE "\r\n" "\n" h2_wdrv_source "${h2_wdrv_source}")
string(SHA256 h2_wdrv_sha256 "${h2_wdrv_source}")
if(NOT h2_wdrv_sha256 STREQUAL "3b444e8800730ff572ba7d46f6c1fea4187e72895bd2e2af2b0a1075ab7bf4dc")
  message(FATAL_ERROR "Pinned BK WDRV source changed; revalidate the SDK correction")
endif()
set(h2_wdrv_signature [=[            WDRV_ENTER_TXMSG_CRITICAL(int_level);
            co_list_push_back((struct co_list *)&wdrv_host_env.cfm_pending_list,(struct co_list_hdr *)&cfm->list);
            WDRV_EXIT_TXMSG_CRITICAL(int_level);]=])
string(REPLACE "${h2_wdrv_signature}" "" h2_wdrv_without "${h2_wdrv_source}")
string(LENGTH "${h2_wdrv_source}" h2_wdrv_before)
string(LENGTH "${h2_wdrv_without}" h2_wdrv_after)
string(LENGTH "${h2_wdrv_signature}" h2_wdrv_expected)
math(EXPR h2_wdrv_removed "${h2_wdrv_before} - ${h2_wdrv_after}")
if(NOT h2_wdrv_removed EQUAL h2_wdrv_expected)
  message(FATAL_ERROR "Pinned BK WDRV confirmation enqueue changed; revalidate the SDK correction")
endif()
set(h2_wdrv_replacement [=[            rtos_lock_mutex(&wdrv_host_env.cfm_lock);
            WDRV_ENTER_TXMSG_CRITICAL(int_level);
            co_list_push_back((struct co_list *)&wdrv_host_env.cfm_pending_list,(struct co_list_hdr *)&cfm->list);
            WDRV_EXIT_TXMSG_CRITICAL(int_level);
            rtos_unlock_mutex(&wdrv_host_env.cfm_lock);]=])
string(REPLACE "${h2_wdrv_signature}" "${h2_wdrv_replacement}"
  h2_wdrv_corrected "${h2_wdrv_source}")
file(WRITE "${H2_BK_WDRV_OUTPUT}" "${h2_wdrv_corrected}")
