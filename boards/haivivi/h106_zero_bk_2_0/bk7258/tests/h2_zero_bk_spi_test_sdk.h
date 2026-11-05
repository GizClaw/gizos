#ifndef H2_ZERO_BK_SPI_TEST_SDK_H
#define H2_ZERO_BK_SPI_TEST_SDK_H
#include <stddef.h>
#include <stdint.h>
#define kNoErr 0
#define kGeneralErr (-1)
typedef void* beken_mutex_t;
typedef uint32_t spi_id_t;
typedef enum { PM_SLEEP_MODULE_NAME_SPI_1 = 1 } pm_sleep_module_name_e;
#ifdef __cplusplus
extern "C" {
#endif
int rtos_init_mutex(beken_mutex_t* mutex);
int rtos_lock_mutex(beken_mutex_t* mutex);
int rtos_unlock_mutex(beken_mutex_t* mutex);
int bk_pm_module_vote_sleep_ctrl(pm_sleep_module_name_e module,
                                 uint32_t sleep_state, uint32_t sleep_time);
int bk_spi_write_bytes(spi_id_t port, const void* data, uint32_t size);
int bk_spi_dma_write_bytes(spi_id_t port, const void* data, uint32_t size);
#ifdef __cplusplus
}
#endif
#endif
