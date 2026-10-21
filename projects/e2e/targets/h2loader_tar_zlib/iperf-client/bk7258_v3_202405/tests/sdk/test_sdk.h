#ifndef H2_BK_IPERF_TEST_SDK_H
#define H2_BK_IPERF_TEST_SDK_H
#include "h2_iperf_client_app.h"
#include <stddef.h>
#include <stdint.h>

#define CONFIG_UART_PRINT_BAUD_RATE 115200u
#define CONFIG_UART_PRINT_PORT 0u
#define H2_LOADER_CAPABILITY_UART 1u
#define H2_LOADER_CAPABILITY_WIFI 2u

typedef struct h2_bk_platform_resource_stats {
  size_t live_tasks, task_stack_bytes;
  size_t live_queues, live_mutexes, live_semaphores, live_conditions;
  size_t live_timers, live_firmware_infos;
  size_t allocations, allocation_bytes;
} h2_bk_platform_resource_stats_t;

void bk_init(void);
void rtos_delay_milliseconds(uint32_t milliseconds);
uint32_t rtos_get_time(void);
size_t rtos_get_free_heap_size(void);
size_t rtos_get_minimum_free_heap_size(void);
size_t rtos_get_psram_free_heap_size(void);
int bk_uart_is_tx_over(uint32_t port);
int h2_bk_target_task_policy_install(void);
h2_pal_result_t h2_bk7258_board_runtime_config(h2_runtime_config_t* config);
h2_pal_result_t h2_bk7258_board_runtime_deinit(void);
h2_pal_result_t h2_bk7258_board_start_entry_task(const char* name,
                                                 void (*entry)(void*),
                                                 void* user);
h2_pal_mem_api_t* h2_bk_platform_psram_allocator(void);
h2_pal_result_t h2_bk_platform_get_resource_stats(
    h2_bk_platform_resource_stats_t* out);
const h2_pal_uart_io_stream_api_t* h2_bk_platform_uart_io_stream_api(void);
void h2_bk_platform_uart_io_stream_deinit(void);
int h2_bk_h2loader_start_app_iostreamikcp_with_capabilities(
    h2_runtime_t* runtime, const char* name, uint32_t capabilities);
int h2_bk_h2loader_stop_app_iostreamikcp(void);
int h2_bk_h2loader_confirm_current_app(h2_runtime_t* runtime);
#endif
