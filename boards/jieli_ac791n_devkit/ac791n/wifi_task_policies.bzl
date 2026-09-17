"""SDK Wi-Fi tasks from apps/demo/demo_DevKitBoard/app_main.c task_info_table.

Rows use jieli_target_task_policy's name priority stack_words queue_words format.
SDK task_create looks up task_info_table and fails for missing names; the PAL
fallback does not cover these SDK-internal driver tasks.
"""

JIELI_AC791N_WIFI_SDK_TASK_POLICIES = [
    "tcpip_thread 16 800 0",
    "tasklet 10 1400 0",
    "RtmpMlmeTask 17 700 0",
    "RtmpCmdQTask 17 300 0",
    "wl_rx_irq_thread 5 256 0",
]
