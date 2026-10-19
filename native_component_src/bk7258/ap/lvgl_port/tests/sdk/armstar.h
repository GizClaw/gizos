#ifndef H2_BK_DMA2D_TEST_CACHE_H
#define H2_BK_DMA2D_TEST_CACHE_H
#include <stdint.h>
typedef struct test_scb { uint32_t CLIDR; } test_scb_t;
extern test_scb_t test_scb;
#define SCB (&test_scb)
#define SCB_CLIDR_DC_Msk 1
void SCB_CleanInvalidateDCache_by_Addr(void *address, long bytes);
#endif
