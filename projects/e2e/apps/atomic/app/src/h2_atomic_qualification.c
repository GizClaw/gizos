#include "h2/pal/core/h2_pal_errors.h"
#include "h2_atomic_e2e.h"
#include "h2_atomic_static.h"
#include <stdio.h>
#include <string.h>

#define CHECK(expression)                                                      \
  do {                                                                         \
    if (!(expression))                                                         \
      return H2_PAL_ERR_INVALID_STATE;                                         \
  } while (0)
static const h2_atomic_order_t orders[] = {H2_ATOMIC_RELAXED, H2_ATOMIC_ACQUIRE,
                                           H2_ATOMIC_RELEASE, H2_ATOMIC_ACQ_REL,
                                           H2_ATOMIC_SEQ_CST};
static h2_atomic_order_t failure_order(h2_atomic_order_t success) {
  if (success == H2_ATOMIC_SEQ_CST)
    return H2_ATOMIC_SEQ_CST;
  if (success == H2_ATOMIC_ACQUIRE || success == H2_ATOMIC_ACQ_REL)
    return H2_ATOMIC_ACQUIRE;
  return H2_ATOMIC_RELAXED;
}

/* Each type exercises every typed operation, every valid RMW order, load/store
 * orders, expected replacement on failed CAS, and C generic dispatch. */
#define INTEGER_TEST(kind, type)                                               \
  H2_ATOMIC_DEFINE_STATIC_ACCESSOR(kind, static_##kind, 0);                    \
  static int operations_##kind(h2_atomic_##kind##_t *v) {                      \
    const h2_atomic_##kind##_t *cv = v;                                        \
    h2_atomic_store(v, (type)3);                                               \
    CHECK(h2_atomic_load(cv) == (type)3);                                      \
    CHECK(h2_atomic_exchange(v, (type)4) == (type)3);                          \
    CHECK(h2_atomic_fetch_add(v, (type)2) == (type)4);                         \
    CHECK(h2_atomic_fetch_sub(v, (type)1) == (type)6);                         \
    CHECK(h2_atomic_fetch_or(v, (type)2) == (type)5);                          \
    CHECK(h2_atomic_fetch_and(v, (type)3) == (type)7);                         \
    type e = 3;                                                                \
    CHECK(h2_atomic_compare_exchange_strong(v, &e, (type)4));                  \
    e = 4;                                                                     \
    CHECK(h2_atomic_compare_exchange_weak_explicit(                            \
        v, &e, (type)5, H2_ATOMIC_ACQ_REL, H2_ATOMIC_ACQUIRE));                \
    e = 5;                                                                     \
    CHECK(h2_atomic_compare_exchange_strong_explicit(                          \
        v, &e, (type)6, H2_ATOMIC_RELEASE, H2_ATOMIC_RELAXED));                \
    for (unsigned i = 0; i < 5u; ++i) {                                        \
      h2_atomic_##kind##_store(v, (type)5, H2_ATOMIC_RELAXED);                 \
      CHECK(h2_atomic_##kind##_load(cv, H2_ATOMIC_RELAXED) == (type)5);        \
      CHECK(h2_atomic_##kind##_exchange(v, (type)6, orders[i]) == (type)5);    \
      CHECK(h2_atomic_##kind##_fetch_add(v, (type)2, orders[i]) == (type)6);   \
      CHECK(h2_atomic_##kind##_fetch_sub(v, (type)1, orders[i]) == (type)8);   \
      CHECK(h2_atomic_##kind##_fetch_or(v, (type)8, orders[i]) == (type)7);    \
      CHECK(h2_atomic_##kind##_fetch_and(v, (type)3, orders[i]) == (type)15);  \
      e = 2;                                                                   \
      CHECK(!h2_atomic_##kind##_compare_exchange(v, &e, (type)4, orders[i],    \
                                                 failure_order(orders[i])));   \
      CHECK(e == (type)3);                                                     \
      CHECK(h2_atomic_##kind##_compare_exchange(v, &e, (type)4, orders[i],     \
                                                failure_order(orders[i])));    \
      e = 4;                                                                   \
      CHECK(h2_atomic_compare_exchange_explicit(v, &e, (type)5, orders[i],     \
                                                H2_ATOMIC_RELAXED));           \
      h2_atomic_store_explicit(v, (type)9, H2_ATOMIC_RELEASE);                 \
      CHECK(h2_atomic_load_explicit(cv, H2_ATOMIC_ACQUIRE) == (type)9);        \
      h2_atomic_##kind##_store(v, (type)10, H2_ATOMIC_SEQ_CST);                \
      CHECK(h2_atomic_##kind##_load(cv, H2_ATOMIC_SEQ_CST) == (type)10);       \
    }                                                                          \
    return H2_PAL_OK;                                                          \
  }                                                                            \
  static int lifecycle_##kind(h2_atomic_##kind##_t *v, bool is_static,         \
                              const h2_atomic_qualification_config_t *c) {     \
    CHECK(h2_atomic_##kind##_init(NULL, (type)0) == H2_ATOMIC_INVALID_ARG);    \
    h2_atomic_##kind##_destroy(NULL);                                          \
    if (!is_static)                                                            \
      CHECK(h2_atomic_init_explicit(v, (type)0) == H2_ATOMIC_OK);              \
    CHECK(h2_atomic_init(v, (type)1) == H2_ATOMIC_INVALID_STATE);              \
    int rc = c->check_placement                                                \
                 ? c->check_placement((uintptr_t)v, (uintptr_t)v->storage,     \
                                      is_static, c->placement_user)            \
                 : 0;                                                          \
    if (!rc)                                                                   \
      rc = operations_##kind(v);                                               \
    h2_atomic_##kind##_destroy(v);                                             \
    if (is_static) {                                                           \
      CHECK(v->storage != NULL);                                               \
      CHECK(h2_atomic_load(v) == (type)10);                                    \
    } else {                                                                   \
      CHECK(v->storage == NULL);                                               \
      h2_atomic_destroy(v);                                                    \
      CHECK(h2_atomic_##kind##_init(v, (type)11) == H2_ATOMIC_OK);             \
      CHECK(h2_atomic_load(v) == (type)11);                                    \
      h2_atomic_destroy_explicit(v);                                           \
      CHECK(v->storage == NULL);                                               \
    }                                                                          \
    return rc;                                                                 \
  }
INTEGER_TEST(int, int)
INTEGER_TEST(uint, unsigned)
INTEGER_TEST(u8, uint8_t)
INTEGER_TEST(u16, uint16_t)
INTEGER_TEST(u32, uint32_t)
INTEGER_TEST(size, size_t)
#undef INTEGER_TEST

H2_ATOMIC_DEFINE_STATIC(bool, static_bool, false);
H2_ATOMIC_DEFINE_STATIC(ptr, static_ptr, NULL);
H2_ATOMIC_DEFINE_STATIC(flag, static_flag, 0u);
static int pointer_targets[2];

static int lifecycle_bool(h2_atomic_bool_t *v, bool is_static,
                          const h2_atomic_qualification_config_t *c) {
  CHECK(h2_atomic_bool_init(NULL, false) == H2_ATOMIC_INVALID_ARG);
  h2_atomic_bool_destroy(NULL);
  if (!is_static)
    CHECK(h2_atomic_init(v, false) == H2_ATOMIC_OK);
  CHECK(h2_atomic_bool_init(v, true) == H2_ATOMIC_INVALID_STATE);
  int placement = c->check_placement
                      ? c->check_placement((uintptr_t)v, (uintptr_t)v->storage,
                                           is_static, c->placement_user)
                      : 0;
  for (unsigned i = 0; i < 5u; ++i) {
    h2_atomic_bool_store(v, false, H2_ATOMIC_RELAXED);
    CHECK(!h2_atomic_bool_load(v, H2_ATOMIC_RELAXED));
    CHECK(!h2_atomic_bool_exchange(v, true, orders[i]));
    bool expected = false;
    CHECK(!h2_atomic_bool_compare_exchange(v, &expected, false, orders[i],
                                           failure_order(orders[i])));
    CHECK(expected);
    CHECK(h2_atomic_compare_exchange_explicit(v, &expected, false, orders[i],
                                              H2_ATOMIC_RELAXED));
    h2_atomic_store_explicit(v, true, H2_ATOMIC_RELEASE);
    CHECK(h2_atomic_load_explicit(v, H2_ATOMIC_ACQUIRE));
    CHECK(h2_atomic_exchange(v, false));
    expected = false;
    CHECK(h2_atomic_compare_exchange_strong(v, &expected, true));
    h2_atomic_store(v, false);
    CHECK(!h2_atomic_load(v));
  }
  h2_atomic_destroy(v);
  if (is_static)
    CHECK(v->storage != NULL && !h2_atomic_load(v));
  else {
    CHECK(v->storage == NULL);
    h2_atomic_bool_destroy(v);
    CHECK(h2_atomic_init_explicit(v, true) == H2_ATOMIC_OK);
    CHECK(h2_atomic_load(v));
    h2_atomic_destroy(v);
    CHECK(v->storage == NULL);
  }
  return placement;
}
static int lifecycle_ptr(h2_atomic_ptr_t *v, bool is_static,
                         const h2_atomic_qualification_config_t *c) {
  CHECK(h2_atomic_ptr_init(NULL, NULL) == H2_ATOMIC_INVALID_ARG);
  h2_atomic_ptr_destroy(NULL);
  if (!is_static)
    CHECK(h2_atomic_init(v, NULL) == H2_ATOMIC_OK);
  CHECK(h2_atomic_ptr_init(v, NULL) == H2_ATOMIC_INVALID_STATE);
  int placement = c->check_placement
                      ? c->check_placement((uintptr_t)v, (uintptr_t)v->storage,
                                           is_static, c->placement_user)
                      : 0;
  for (unsigned i = 0; i < 5u; ++i) {
    h2_atomic_ptr_store(v, NULL, H2_ATOMIC_RELAXED);
    CHECK(h2_atomic_ptr_load(v, H2_ATOMIC_RELAXED) == NULL);
    CHECK(h2_atomic_ptr_exchange(v, &pointer_targets[0], orders[i]) == NULL);
    void *expected = NULL;
    CHECK(!h2_atomic_ptr_compare_exchange(v, &expected, &pointer_targets[1],
                                          orders[i], failure_order(orders[i])));
    CHECK(expected == &pointer_targets[0]);
    CHECK(h2_atomic_compare_exchange_explicit(v, &expected, &pointer_targets[1],
                                              orders[i], H2_ATOMIC_RELAXED));
    h2_atomic_store_explicit(v, &pointer_targets[0], H2_ATOMIC_RELEASE);
    CHECK(h2_atomic_load_explicit(v, H2_ATOMIC_ACQUIRE) == &pointer_targets[0]);
    CHECK(h2_atomic_exchange(v, NULL) == &pointer_targets[0]);
    expected = NULL;
    CHECK(h2_atomic_compare_exchange_strong(v, &expected, &pointer_targets[0]));
    h2_atomic_store(v, NULL);
    CHECK(h2_atomic_load(v) == NULL);
  }
  h2_atomic_destroy(v);
  if (is_static)
    CHECK(v->storage != NULL && h2_atomic_load(v) == NULL);
  else {
    CHECK(v->storage == NULL);
    h2_atomic_ptr_destroy(v);
    CHECK(h2_atomic_init_explicit(v, NULL) == H2_ATOMIC_OK);
    h2_atomic_destroy(v);
    CHECK(v->storage == NULL);
  }
  return placement;
}
static int lifecycle_flag(h2_atomic_flag_t *v, bool is_static,
                          const h2_atomic_qualification_config_t *c) {
  CHECK(h2_atomic_flag_init(NULL) == H2_ATOMIC_INVALID_ARG);
  h2_atomic_flag_destroy(NULL);
  if (!is_static)
    CHECK(h2_atomic_flag_init(v) == H2_ATOMIC_OK);
  CHECK(h2_atomic_flag_init(v) == H2_ATOMIC_INVALID_STATE);
  int placement = c->check_placement
                      ? c->check_placement((uintptr_t)v, (uintptr_t)v->storage,
                                           is_static, c->placement_user)
                      : 0;
  for (unsigned i = 0; i < 5u; ++i) {
    CHECK(!h2_atomic_flag_test_and_set(v, orders[i]));
    CHECK(h2_atomic_flag_test_and_set(v, orders[i]));
    h2_atomic_flag_clear(v, H2_ATOMIC_RELEASE);
    CHECK(!h2_atomic_flag_test_and_set(v, H2_ATOMIC_ACQUIRE));
    h2_atomic_flag_clear(v, H2_ATOMIC_RELAXED);
  }
  h2_atomic_flag_clear(v, H2_ATOMIC_SEQ_CST);
  h2_atomic_destroy(v);
  if (is_static)
    CHECK(v->storage != NULL);
  else {
    CHECK(v->storage == NULL);
    h2_atomic_flag_destroy(v);
    CHECK(h2_atomic_flag_init(v) == H2_ATOMIC_OK);
    h2_atomic_flag_destroy(v);
    CHECK(v->storage == NULL);
  }
  return placement;
}

typedef struct qualification_state qualification_state_t;
typedef struct qualification_worker {
  qualification_state_t *state;
  unsigned index;
  int rc;
  int core;
} qualification_worker_t;
struct qualification_state {
  h2_atomic_qualification_config_t config;
  h2_atomic_uint_t ready, done;
  h2_atomic_bool_t go, cancel, signal;
  h2_atomic_ptr_t published;
  h2_atomic_flag_t lock;
  h2_atomic_int_t ints, int_cas;
  h2_atomic_uint_t uints, uint_cas;
  h2_atomic_u8_t u8s, u8_cas;
  h2_atomic_u16_t u16s, u16_cas;
  h2_atomic_u32_t u32s, u32_cas;
  h2_atomic_size_t sizes, size_cas;
  unsigned protected_count, protected_complement, payload, round;
  unsigned mode;
  qualification_worker_t workers[2];
};
static void pause_worker(qualification_state_t *s) {
  (void)h2_pal_time_sleep_ms(s->config.time, 1);
}
static bool cancelled(qualification_state_t *s) {
  return h2_atomic_bool_load(&s->cancel, H2_ATOMIC_ACQUIRE);
}
#define CONTENTION(kind, type, member, count)                                  \
  for (unsigned n = 0; n < (count); ++n) {                                     \
    (void)h2_atomic_##kind##_fetch_add(&s->member, (type)1,                    \
                                       H2_ATOMIC_RELAXED);                     \
    (void)h2_atomic_##kind##_fetch_add(static_##kind(), (type)1,               \
                                       H2_ATOMIC_RELAXED);                     \
    type e = h2_atomic_##kind##_load(&s->kind##_cas, H2_ATOMIC_RELAXED);       \
    unsigned attempts = 0;                                                     \
    while (!h2_atomic_##kind##_compare_exchange(                               \
        &s->kind##_cas, &e, (type)(e + 1), H2_ATOMIC_ACQ_REL,                  \
        H2_ATOMIC_ACQUIRE)) {                                                  \
      if (++attempts == 1000000 || cancelled(s))                               \
        return H2_PAL_ERR_TIMEOUT;                                             \
    }                                                                          \
  }
static int concurrent_work(qualification_worker_t *w) {
  qualification_state_t *s = w->state;
  if (s->mode == 0) {
    CONTENTION(int, int, ints, 5000u)
    CONTENTION(uint, unsigned, uints, 5000u)
    CONTENTION(u8, uint8_t, u8s, 100u)
    CONTENTION(u16, uint16_t, u16s, 5000u)
    CONTENTION(u32, uint32_t, u32s, 5000u)
    CONTENTION(size, size_t, sizes, 5000u)
  } else if (s->mode == 1) {
    for (unsigned n = 0; n < 1000u; ++n) {
      while (h2_atomic_flag_test_and_set(&s->lock, H2_ATOMIC_ACQUIRE)) {
        if (cancelled(s))
          return H2_PAL_ERR_TIMEOUT;
        pause_worker(s);
      }
      bool valid = s->protected_complement == ~s->protected_count;
      ++s->protected_count;
      s->protected_complement = ~s->protected_count;
      h2_atomic_flag_clear(&s->lock, H2_ATOMIC_RELEASE);
      CHECK(valid);
    }
  } else {
    for (unsigned n = 1; n <= 128u; ++n) {
      if (w->index == 0) {
        while (h2_atomic_bool_load(&s->signal, H2_ATOMIC_ACQUIRE)) {
          if (cancelled(s))
            return H2_PAL_ERR_TIMEOUT;
          pause_worker(s);
        }
        s->payload = n * 17u;
        s->round = n;
        if (s->mode == 3)
          h2_atomic_ptr_store(&s->published, &s->payload, H2_ATOMIC_RELEASE);
        h2_atomic_bool_store(&s->signal, true, H2_ATOMIC_RELEASE);
      } else {
        while (!h2_atomic_bool_load(&s->signal, H2_ATOMIC_ACQUIRE)) {
          if (cancelled(s))
            return H2_PAL_ERR_TIMEOUT;
          pause_worker(s);
        }
        unsigned *p = s->mode == 3
                          ? h2_atomic_ptr_load(&s->published, H2_ATOMIC_ACQUIRE)
                          : &s->payload;
        CHECK(p == &s->payload && *p == n * 17u && s->round == n);
        h2_atomic_bool_store(&s->signal, false, H2_ATOMIC_RELEASE);
      }
    }
  }
  return H2_PAL_OK;
}
#undef CONTENTION
static void concurrent_entry(void *user) {
  qualification_worker_t *w = user;
  qualification_state_t *s = w->state;
  w->core =
      s->config.current_core ? s->config.current_core(s->config.core_user) : -1;
  h2_atomic_uint_fetch_add(&s->ready, 1, H2_ATOMIC_RELEASE);
  while (!h2_atomic_bool_load(&s->go, H2_ATOMIC_ACQUIRE))
    pause_worker(s);
  w->rc = cancelled(s) ? H2_PAL_ERR_TIMEOUT : concurrent_work(w);
  if (w->rc)
    h2_atomic_bool_store(&s->cancel, true, H2_ATOMIC_RELEASE);
  h2_atomic_uint_fetch_add(&s->done, 1, H2_ATOMIC_RELEASE);
}
static int wait_count(qualification_state_t *s, h2_atomic_uint_t *count,
                      unsigned target) {
  uint64_t start = 0, now = 0;
  int rc = h2_pal_time_get_monotonic_us(s->config.time, &start);
  if (rc)
    return rc;
  while (h2_atomic_uint_load(count, H2_ATOMIC_ACQUIRE) != target) {
    if (s->config.pump)
      s->config.pump(s->config.pump_user);
    pause_worker(s);
    rc = h2_pal_time_get_monotonic_us(s->config.time, &now);
    if (rc)
      return rc;
    if (now - start >= 10000000u)
      return H2_PAL_ERR_TIMEOUT;
  }
  return H2_PAL_OK;
}
static int run_workers(qualification_state_t *s,
                       h2_atomic_qualification_result_t *r) {
  h2_atomic_uint_store(&s->ready, 0, H2_ATOMIC_RELAXED);
  h2_atomic_uint_store(&s->done, 0, H2_ATOMIC_RELAXED);
  h2_atomic_bool_store(&s->go, false, H2_ATOMIC_RELAXED);
  h2_atomic_bool_store(&s->cancel, false, H2_ATOMIC_RELAXED);
  h2_pal_task_t *handles[2] = {0};
  unsigned started = 0;
  int rc = H2_PAL_OK;
  for (unsigned i = 0; i < 2; ++i) {
    s->workers[i] = (qualification_worker_t){s, i, 0, -1};
    const h2_pal_task_options_t options = {.name = i ? "atomic/e2e/core1"
                                                     : "atomic/e2e/core0",
                                           .min_stack_size = 8192};
    rc = h2_pal_task_start(s->config.task, &options, concurrent_entry,
                           &s->workers[i], &handles[i]);
    if (rc)
      break;
    ++started;
    ++r->workers_started;
  }
  if (!rc)
    rc = wait_count(s, &s->ready, 2);
  if (rc)
    h2_atomic_bool_store(&s->cancel, true, H2_ATOMIC_RELEASE);
  h2_atomic_bool_store(&s->go, true, H2_ATOMIC_RELEASE);
  int done_rc = wait_count(s, &s->done, started);
  if (done_rc) {
    h2_atomic_bool_store(&s->cancel, true, H2_ATOMIC_RELEASE);
    done_rc = wait_count(s, &s->done, started);
  }
  if (done_rc) {
    r->teardown = done_rc;
    return done_rc;
  }
  for (unsigned i = 0; i < started; ++i) {
    int join = H2_PAL_ERR_WOULD_BLOCK;
    for (unsigned retry = 0; retry < 3000u; ++retry) {
      join = h2_pal_task_join(s->config.task, handles[i]);
      if (join != H2_PAL_ERR_WOULD_BLOCK && join != H2_PAL_ERR_BUSY)
        break;
      if (s->config.pump)
        s->config.pump(s->config.pump_user);
      pause_worker(s);
    }
    if (join) {
      r->teardown = join;
      return join;
    }
    ++r->workers_joined;
    r->worker_core[i] = s->workers[i].core;
    if (!rc)
      rc = s->workers[i].rc;
    if (!rc && s->config.expected_core[i] >= 0 &&
        s->workers[i].core != s->config.expected_core[i])
      rc = H2_PAL_ERR_INVALID_STATE;
  }
  if (!rc && s->config.require_distinct_workers &&
      r->worker_core[0] == r->worker_core[1])
    rc = H2_PAL_ERR_INVALID_STATE;
  return rc;
}
static void record(h2_atomic_qualification_result_t *r, unsigned index,
                   int rc) {
  r->cases[index].rc = rc;
  r->cases[index].status = rc ? 2 : 1;
  if (rc)
    ++r->failed;
  else
    ++r->passed;
  --r->not_run;
}
static void destroy_state(qualification_state_t *s) {
#define DESTROY(v) h2_atomic_destroy(&s->v)
  DESTROY(ready);
  DESTROY(done);
  DESTROY(go);
  DESTROY(cancel);
  DESTROY(signal);
  DESTROY(published);
  DESTROY(lock);
  DESTROY(ints);
  DESTROY(int_cas);
  DESTROY(uints);
  DESTROY(uint_cas);
  DESTROY(u8s);
  DESTROY(u8_cas);
  DESTROY(u16s);
  DESTROY(u16_cas);
  DESTROY(u32s);
  DESTROY(u32_cas);
  DESTROY(sizes);
  DESTROY(size_cas);
#undef DESTROY
}
int h2_atomic_e2e_qualify(const h2_atomic_qualification_config_t *c,
                          h2_atomic_qualification_result_t *r) {
  if (!r)
    return H2_PAL_ERR_INVALID_ARG;
  memset(r, 0, sizeof(*r));
  static const char *ids[] = {
#define H2_ATOMIC_CASE(id) id,
#include "h2_atomic_cases.inc"
#undef H2_ATOMIC_CASE
  };
  _Static_assert(sizeof(ids) / sizeof(ids[0]) ==
                     H2_ATOMIC_QUALIFICATION_CASE_COUNT,
                 "registry count");
  for (unsigned i = 0; i < H2_ATOMIC_QUALIFICATION_CASE_COUNT; ++i)
    r->cases[i].id = ids[i];
  r->not_run = H2_ATOMIC_QUALIFICATION_CASE_COUNT;
  if (!c || !c->mem || !c->task || !c->time)
    return H2_PAL_ERR_INVALID_ARG;
  unsigned index = 0;
#define LIFECYCLE(kind, pointer)                                               \
  do {                                                                         \
    h2_atomic_##kind##_t *dynamic =                                            \
        h2_pal_mem_alloc(c->mem, sizeof(*dynamic));                            \
    int rc = H2_PAL_ERR_NO_MEMORY;                                             \
    if (dynamic) {                                                             \
      memset(dynamic, 0, sizeof(*dynamic));                                    \
      rc = lifecycle_##kind(dynamic, false, c);                                \
      h2_atomic_##kind##_destroy(dynamic);                                     \
      h2_pal_mem_free(c->mem, dynamic);                                        \
    }                                                                          \
    record(r, index, rc);                                                      \
    record(r, index + 9, lifecycle_##kind(pointer, true, c));                  \
    ++index;                                                                   \
  } while (0)
  LIFECYCLE(int, static_int());
  LIFECYCLE(uint, static_uint());
  LIFECYCLE(u8, static_u8());
  LIFECYCLE(u16, static_u16());
  LIFECYCLE(u32, static_u32());
  LIFECYCLE(size, static_size());
  LIFECYCLE(bool, &static_bool);
  LIFECYCLE(ptr, &static_ptr);
  LIFECYCLE(flag, &static_flag);
#undef LIFECYCLE
  qualification_state_t *s = h2_pal_mem_alloc(c->mem, sizeof(*s));
  if (!s)
    return H2_PAL_ERR_NO_MEMORY;
  memset(s, 0, sizeof(*s));
  s->config = *c;
  s->protected_complement = ~0u;
  int init = 0;
#define INIT(v, initial)                                                       \
  do {                                                                         \
    if (h2_atomic_init(&s->v, initial) != H2_ATOMIC_OK)                        \
      init = H2_PAL_ERR_NO_MEMORY;                                             \
  } while (0)
  INIT(ready, 0);
  INIT(done, 0);
  INIT(go, false);
  INIT(cancel, false);
  INIT(signal, false);
  INIT(published, NULL);
  INIT(ints, 0);
  INIT(int_cas, 0);
  INIT(uints, 0);
  INIT(uint_cas, 0);
  INIT(u8s, 0);
  INIT(u8_cas, 0);
  INIT(u16s, 0);
  INIT(u16_cas, 0);
  INIT(u32s, 0);
  INIT(u32_cas, 0);
  INIT(sizes, 0);
  INIT(size_cas, 0);
#undef INIT
  if (h2_atomic_flag_init(&s->lock) != H2_ATOMIC_OK)
    init = H2_PAL_ERR_NO_MEMORY;
  if (init) {
    destroy_state(s);
    h2_pal_mem_free(c->mem, s);
    return init;
  }
  h2_atomic_store(static_int(), 0);
  h2_atomic_store(static_uint(), 0);
  h2_atomic_store(static_u8(), 0);
  h2_atomic_store(static_u16(), 0);
  h2_atomic_store(static_u32(), 0);
  h2_atomic_store(static_size(), 0);
  int work = run_workers(s, r);
#define COUNTER(index, kind, member, expected)                                 \
  record(r, index,                                                             \
         work ? work                                                           \
              : (h2_atomic_load(&s->member) == (expected) &&                   \
                         h2_atomic_load(&s->kind##_cas) == (expected) &&       \
                         h2_atomic_load(static_##kind()) == (expected)         \
                     ? 0                                                       \
                     : H2_PAL_ERR_INVALID_STATE))
  COUNTER(18, int, ints, 10000);
  COUNTER(19, uint, uints, 10000u);
  COUNTER(20, u8, u8s, 200u);
  COUNTER(21, u16, u16s, 10000u);
  COUNTER(22, u32, u32s, 10000u);
  COUNTER(23, size, sizes, 10000u);
#undef COUNTER
  if (r->teardown)
    return r->teardown;
  for (s->mode = 1; s->mode <= 3; ++s->mode) {
    work = run_workers(s, r);
    if (!work && s->mode == 1 && s->protected_count != 2000u)
      work = H2_PAL_ERR_INVALID_STATE;
    if (!work && s->mode >= 2 &&
        (s->round != 128 || h2_atomic_bool_load(&s->signal, H2_ATOMIC_ACQUIRE)))
      work = H2_PAL_ERR_INVALID_STATE;
    record(r, 23 + s->mode, work);
    if (r->teardown)
      return r->teardown;
  }
  destroy_state(s);
  h2_pal_mem_free(c->mem, s);
  h2_atomic_flag_e2e_result_t flags;
  int flag_rc = h2_atomic_flag_e2e_run(c->mem, c->task, c->time, 2000, NULL,
                                       NULL, &flags);
  record(r, 27, flag_rc);
  r->workers_started += flags.workers_started;
  r->workers_joined += flags.workers_joined;
  if (flags.teardown)
    r->teardown = flags.teardown;
  r->complete = r->not_run == 0;
  r->qualified = r->complete && !r->failed && !r->teardown &&
                 r->workers_started == r->workers_joined;
  return r->qualified ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
void h2_atomic_e2e_print(const char *platform, const char *placement,
                         const h2_atomic_qualification_result_t *r) {
  for (unsigned i = 0; i < H2_ATOMIC_QUALIFICATION_CASE_COUNT; ++i)
    printf("ATOMIC_CASE platform=%s placement=%s id=%s status=%s rc=%d\n",
           platform, placement, r->cases[i].id,
           r->cases[i].status == 1   ? "PASS"
           : r->cases[i].status == 2 ? "FAIL"
                                     : "NOT_RUN",
           r->cases[i].rc);
  printf("ATOMIC_QUALIFICATION platform=%s placement=%s passed=%u failed=%u "
         "not_run=%u workers=%u/%u cores=%d,%d complete=%u qualified=%u "
         "teardown=%d\n",
         platform, placement, r->passed, r->failed, r->not_run,
         r->workers_joined, r->workers_started, r->worker_core[0],
         r->worker_core[1], r->complete, r->qualified, r->teardown);
}
