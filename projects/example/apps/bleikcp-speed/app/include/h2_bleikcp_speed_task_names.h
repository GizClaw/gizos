#ifndef H2_BLEIKCP_SPEED_TASK_NAMES_H
#define H2_BLEIKCP_SPEED_TASK_NAMES_H

#define H2_BLEIKCP_SPEED_TASK_PREFIX_VALUE "bleikcp-speed/"
#define H2_BLEIKCP_SPEED_KCP_TASK_NAME_VALUE "bleikcp-speed/kcp"
#define H2_BLEIKCP_SPEED_SERVER_TASK_NAME_VALUE "bleikcp-speed/server"
#define H2_BLEIKCP_SPEED_UI_TASK_NAME_VALUE "bleikcp-speed/ui"
#define H2_BLEIKCP_SPEED_TX_TASK_NAME_VALUE "bleikcp-speed/tx"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_bleikcp_speed_task_prefix[sizeof(H2_BLEIKCP_SPEED_TASK_PREFIX_VALUE)];
extern const char h2_bleikcp_speed_kcp_task_name[sizeof(
    H2_BLEIKCP_SPEED_KCP_TASK_NAME_VALUE)];
extern const char h2_bleikcp_speed_server_task_name[sizeof(
    H2_BLEIKCP_SPEED_SERVER_TASK_NAME_VALUE)];
extern const char
    h2_bleikcp_speed_ui_task_name[sizeof(H2_BLEIKCP_SPEED_UI_TASK_NAME_VALUE)];
extern const char
    h2_bleikcp_speed_tx_task_name[sizeof(H2_BLEIKCP_SPEED_TX_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
