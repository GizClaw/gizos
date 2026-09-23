#include "h2_bk3633_platform_core.h"
#include <stdatomic.h>
_Static_assert(sizeof(int) == sizeof(uint32_t), "i32 atomic requires 32-bit int");
#if defined(BK3633)
#include "arch.h"
#endif

/* ARMv5 uses cooperative libco scheduling; mask IRQ/FIQ for ISR overlap.
 * PSRAM and multicore behavior still needs hardware verification. */
static uint32_t h2_bk3633_atomic_enter(void) {
#if defined(BK3633)
    uint32_t state = __disable_fiq() != 0 ? 1u : 0u;
    if (__disable_irq() != 0) state |= 2u;
    atomic_signal_fence(memory_order_acquire);
    return state;
#else
    return 0u;
#endif
}
static void h2_bk3633_atomic_exit(uint32_t state) {
#if defined(BK3633)
    atomic_signal_fence(memory_order_release);
    if ((state & 1u) == 0u) __enable_fiq();
    if ((state & 2u) == 0u) __enable_irq();
#else
    (void)state;
#endif
}

static h2_pal_result_t h2_bk3633_u32_load(void * user, const h2_pal_atomic_u32_t * value, uint32_t * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_value = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_store(void * user, h2_pal_atomic_u32_t * value, uint32_t desired, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_exchange(void * user, h2_pal_atomic_u32_t * value, uint32_t desired, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_compare_exchange(void * user, h2_pal_atomic_u32_t * value, uint32_t * expected, uint32_t desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    (void)order;
    (void)failure_order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_exchanged = (*cell == *expected);
    if (*out_exchanged) *cell = desired;
    else *expected = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_fetch_add(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (uint32_t)(*cell + operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_fetch_sub(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (uint32_t)(*cell - operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_fetch_or(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (uint32_t)(*cell | operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_u32_fetch_and(void * user, h2_pal_atomic_u32_t * value, uint32_t operand, uint32_t * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile uint32_t *cell = (volatile uint32_t *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (uint32_t)(*cell & operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_load(void * user, const h2_pal_atomic_i32_t * value, int * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_value = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_store(void * user, h2_pal_atomic_i32_t * value, int desired, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_exchange(void * user, h2_pal_atomic_i32_t * value, int desired, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_compare_exchange(void * user, h2_pal_atomic_i32_t * value, int * expected, int desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    (void)order;
    (void)failure_order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_exchanged = (*cell == *expected);
    if (*out_exchanged) *cell = desired;
    else *expected = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_fetch_add(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (int)((uint32_t)*cell + (uint32_t)operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_fetch_sub(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (int)((uint32_t)*cell - (uint32_t)operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_fetch_or(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (int)(*cell | operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_i32_fetch_and(void * user, h2_pal_atomic_i32_t * value, int operand, int * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile int *cell = (volatile int *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = (int)(*cell & operand);
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_bool_load(void * user, const h2_pal_atomic_bool_t * value, bool * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile bool *cell = (volatile bool *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_value = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_bool_store(void * user, h2_pal_atomic_bool_t * value, bool desired, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile bool *cell = (volatile bool *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_bool_exchange(void * user, h2_pal_atomic_bool_t * value, bool desired, bool * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile bool *cell = (volatile bool *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_bool_compare_exchange(void * user, h2_pal_atomic_bool_t * value, bool * expected, bool desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    (void)order;
    (void)failure_order;
    volatile bool *cell = (volatile bool *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_exchanged = (*cell == *expected);
    if (*out_exchanged) *cell = desired;
    else *expected = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_ptr_load(void * user, const h2_pal_atomic_ptr_t * value, void * * out_value, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    void * volatile *cell = (void * volatile *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_value = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_ptr_store(void * user, h2_pal_atomic_ptr_t * value, void * desired, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    void * volatile *cell = (void * volatile *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_ptr_exchange(void * user, h2_pal_atomic_ptr_t * value, void * desired, void * * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    void * volatile *cell = (void * volatile *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = desired;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_ptr_compare_exchange(void * user, h2_pal_atomic_ptr_t * value, void * * expected, void * desired, bool * out_exchanged, h2_pal_atomic_order_t order, h2_pal_atomic_order_t failure_order) {
    (void)user;
    (void)order;
    (void)failure_order;
    void * volatile *cell = (void * volatile *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_exchanged = (*cell == *expected);
    if (*out_exchanged) *cell = desired;
    else *expected = *cell;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_flag_test_and_set(void * user, h2_pal_atomic_flag_t * value, bool * out_previous, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile bool *cell = (volatile bool *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *out_previous = *cell;
    *cell = true;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static h2_pal_result_t h2_bk3633_flag_clear(void * user, h2_pal_atomic_flag_t * value, h2_pal_atomic_order_t order) {
    (void)user;
    (void)order;
    volatile bool *cell = (volatile bool *)&value->storage;
    uint32_t state = h2_bk3633_atomic_enter();
    *cell = false;
    h2_bk3633_atomic_exit(state);
    return H2_PAL_OK;
}

static const h2_pal_atomic_vtable_t s_vtable = {
    .u32_load = h2_bk3633_u32_load,
    .u32_store = h2_bk3633_u32_store,
    .u32_exchange = h2_bk3633_u32_exchange,
    .u32_compare_exchange = h2_bk3633_u32_compare_exchange,
    .u32_fetch_add = h2_bk3633_u32_fetch_add,
    .u32_fetch_sub = h2_bk3633_u32_fetch_sub,
    .u32_fetch_or = h2_bk3633_u32_fetch_or,
    .u32_fetch_and = h2_bk3633_u32_fetch_and,
    .i32_load = h2_bk3633_i32_load,
    .i32_store = h2_bk3633_i32_store,
    .i32_exchange = h2_bk3633_i32_exchange,
    .i32_compare_exchange = h2_bk3633_i32_compare_exchange,
    .i32_fetch_add = h2_bk3633_i32_fetch_add,
    .i32_fetch_sub = h2_bk3633_i32_fetch_sub,
    .i32_fetch_or = h2_bk3633_i32_fetch_or,
    .i32_fetch_and = h2_bk3633_i32_fetch_and,
    .bool_load = h2_bk3633_bool_load,
    .bool_store = h2_bk3633_bool_store,
    .bool_exchange = h2_bk3633_bool_exchange,
    .bool_compare_exchange = h2_bk3633_bool_compare_exchange,
    .ptr_load = h2_bk3633_ptr_load,
    .ptr_store = h2_bk3633_ptr_store,
    .ptr_exchange = h2_bk3633_ptr_exchange,
    .ptr_compare_exchange = h2_bk3633_ptr_compare_exchange,
    .flag_test_and_set = h2_bk3633_flag_test_and_set,
    .flag_clear = h2_bk3633_flag_clear,
};
static const h2_pal_atomic_api_t s_api = { .user = NULL, .vtable = &s_vtable };
const h2_pal_atomic_api_t *h2_bk3633_platform_atomic_api(void) {
    return &s_api;
}
