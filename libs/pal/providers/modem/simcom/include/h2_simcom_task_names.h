#ifndef H2_SIMCOM_TASK_NAMES_H
#define H2_SIMCOM_TASK_NAMES_H

#define H2_SIMCOM_PPP_RX_TASK_NAME_VALUE "$net/modem_ppp_rx"
#define H2_SIMCOM_CALL_INPUT_TASK_NAME_VALUE "$modem/call_in"
#define H2_SIMCOM_GNSS_FIX_TASK_NAME_VALUE "$modem/gnss_fix"

#ifdef __cplusplus
extern "C" {
#endif

extern const char
    h2_simcom_ppp_rx_task_name[sizeof(H2_SIMCOM_PPP_RX_TASK_NAME_VALUE)];
extern const char h2_simcom_call_input_task_name[sizeof(
    H2_SIMCOM_CALL_INPUT_TASK_NAME_VALUE)];
extern const char
    h2_simcom_gnss_fix_task_name[sizeof(H2_SIMCOM_GNSS_FIX_TASK_NAME_VALUE)];

#ifdef __cplusplus
}
#endif

#endif
