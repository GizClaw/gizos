#ifndef H2_BK_TASK_TLS_H
#define H2_BK_TASK_TLS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* GCC emutls ABI: machine-word size/alignment, location, template pointer. */
typedef struct h2_bk_emutls_control {
    uintptr_t size;
    uintptr_t alignment;
    uintptr_t location;
    const void *initial_value;
} h2_bk_emutls_control_t;

void *__wrap___emutls_get_address(h2_bk_emutls_control_t *control);
int *__wrap___errno(void);
void h2_bk_task_tls_cleanup(void *task);

#ifdef __cplusplus
}
#endif

#endif
