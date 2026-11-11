#ifndef H2_BK_TLS_LIFETIME_H
#define H2_BK_TLS_LIFETIME_H

#include "net/h2_pal_net.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BK_TLS_TCP_MAX 64u
#define H2_BK_TLS_JOB_MAX 8u
#define H2_BK_TLS_HOST_CAPACITY 256u
#define H2_BK_TLS_ALPN_MAX 2u
#define H2_BK_TLS_ALPN_CAPACITY 16u
/* Inline fast-path storage; larger bundles use worker-owned allocation. */
#define H2_BK_TLS_CA_CAPACITY (16u * 1024u)

/* Native-private coordinator. All callbacks/user storage outlive the manager.
 * enter/leave provide a short cross-core metadata guard. clock_ms is monotonic,
 * bounded and safe inside that guard. wait_ms waits/yields for at most its
 * finite argument; wake_worker is nonblocking. Neither may acquire the guard.
 *
 * Up to JOB_MAX separately prestarted workers may call worker_step
 * concurrently; distinct work is claimed under the guard. wrap runs all
 * synchronous TLS setup/handshake/publication on its worker; it must use only
 * the supplied owned config and deadline, and must never physically close fd.
 * It may use bio_canceled to stop between native calls. dispose removes only
 * TLS context (a no-op if none exists); close_fd consumes the native
 * descriptor. These callbacks may block and always run outside the metadata
 * guard. wrap must not retain config or its pointed-to storage after returning.
 *
 * alloc_root/free_root are an optional pair for roots larger than inline
 * storage. Allocation receives only the required byte count (PEM length + 1),
 * never a caller pointer, and returns NULL for allocation failure. Allocation
 * and free run only on workers, outside the guard. Owned storage is released
 * only after preparation, wrap and the caller's waiter have stopped using it.
 * A blocked allocator/free keeps a native fd lifetime pin through completion.
 */
typedef struct h2_bk_tls_lifetime_ops {
  void* user;
  uintptr_t (*enter)(void* user);
  void (*leave)(void* user, uintptr_t guard);
  uint64_t (*clock_ms)(void* user);
  void (*wait_ms)(void* user, uint32_t timeout_ms);
  void (*wake_worker)(void* user);
  h2_pal_result_t (*wrap)(void* user, int fd,
                          const h2_pal_net_tls_config_t* config,
                          uint64_t deadline_ms);
  void (*dispose)(void* user, int fd);
  void (*close_fd)(void* user, int fd);
  void* (*alloc_root)(void* user, size_t bytes);
  void (*free_root)(void* user, void* root);
} h2_bk_tls_lifetime_ops_t;

typedef struct h2_bk_tls_tcp_ref {
  uint32_t generation;
  size_t index;
} h2_bk_tls_tcp_ref_t;

/* Storage is exposed only in this native-private header to permit static
 * allocation. Fields are coordinator-owned; never inspect/mutate them directly.
 * Initialize outside bounded callbacks, and never reinitialize a live manager.
 */
typedef enum h2_bk_tls_tcp_state {
  H2_BK_TLS_TCP_FREE = 0,
  H2_BK_TLS_TCP_PLAIN,
  H2_BK_TLS_TCP_LEGACY,
  H2_BK_TLS_TCP_CONFIGURING,
  H2_BK_TLS_TCP_ACTIVE,
  H2_BK_TLS_TCP_FAILED,
  H2_BK_TLS_TCP_CLOSED,
} h2_bk_tls_tcp_state_t;

typedef struct h2_bk_tls_tcp_record {
  int fd;
  uint32_t generation;
  uint32_t refs;
  int job;
  h2_bk_tls_tcp_state_t state;
  bool caller_owned;
  bool context_present;
  bool reaping;
} h2_bk_tls_tcp_record_t;

typedef enum h2_bk_tls_job_state {
  H2_BK_TLS_JOB_FREE = 0,
  H2_BK_TLS_JOB_PREPARING,
  H2_BK_TLS_JOB_ALLOC_QUEUED,
  H2_BK_TLS_JOB_ALLOCATING,
  H2_BK_TLS_JOB_COPY_READY,
  H2_BK_TLS_JOB_COPYING,
  H2_BK_TLS_JOB_QUEUED,
  H2_BK_TLS_JOB_RUNNING,
  H2_BK_TLS_JOB_DONE,
  H2_BK_TLS_JOB_ACTIVE,
} h2_bk_tls_job_state_t;

typedef struct h2_bk_tls_job {
  h2_bk_tls_job_state_t state;
  size_t tcp_index;
  uint64_t deadline_ms;
  h2_pal_result_t result;
  bool waiter;
  bool abandoned;
  bool preparing;
  uint8_t* dynamic_ca;
  h2_pal_net_tls_config_t config;
  char hostname[H2_BK_TLS_HOST_CAPACITY];
  char alpn[H2_BK_TLS_ALPN_MAX][H2_BK_TLS_ALPN_CAPACITY];
  uint8_t ca[H2_BK_TLS_CA_CAPACITY + 1u];
} h2_bk_tls_job_t;

typedef struct h2_bk_tls_lifetime {
  h2_bk_tls_lifetime_ops_t ops;
  h2_bk_tls_tcp_record_t tcp[H2_BK_TLS_TCP_MAX];
  h2_bk_tls_job_t jobs[H2_BK_TLS_JOB_MAX];
  uint32_t generation;
  bool initialized;
  bool worker_ready;
} h2_bk_tls_lifetime_t;

h2_pal_result_t h2_bk_tls_lifetime_init(h2_bk_tls_lifetime_t* manager,
                                        const h2_bk_tls_lifetime_ops_t* ops);
/* Admission only: false does not cancel jobs or prevent worker_step draining.
 * Set true only after the worker and all dependencies are ready. */
void h2_bk_tls_lifetime_set_worker_ready(h2_bk_tls_lifetime_t* manager,
                                         bool ready);

/* Register before publishing an opened/accepted TCP fd. Registration failure
 * leaves that unpublished native fd owned by its opener. Once registered, every
 * native TCP operation must hold a ref, and every close must use tcp_close. */
h2_pal_result_t h2_bk_tls_tcp_register(h2_bk_tls_lifetime_t* manager, int fd);
h2_pal_result_t h2_bk_tls_tcp_acquire(h2_bk_tls_lifetime_t* manager, int fd,
                                      h2_bk_tls_tcp_ref_t* out_ref,
                                      bool* out_tls);
h2_pal_result_t h2_bk_tls_tcp_release(h2_bk_tls_lifetime_t* manager,
                                      h2_bk_tls_tcp_ref_t* ref);

/* Exclusive gate for the calling thread's compatibility TLS implementation.
 * begin requires an idle plain socket; finish consumes the ref, returns the
 * supplied result (or CLOSED after concurrent close), and marks failed sockets
 * unusable until caller close. Native setup must not close fd. The worker owns
 * context-only cleanup after finish, including failure. */
h2_pal_result_t h2_bk_tls_legacy_begin(h2_bk_tls_lifetime_t* manager, int fd,
                                       h2_bk_tls_tcp_ref_t* out_ref);
h2_pal_result_t h2_bk_tls_legacy_finish(h2_bk_tls_lifetime_t* manager,
                                        h2_bk_tls_tcp_ref_t* ref,
                                        h2_pal_result_t result);

/* Consume caller ownership promptly. Never waits, disposes TLS or calls native
 * close. Already logically closed records are idempotent until reaped.
 * NOT_FOUND identifies an unregistered fd (e.g. UDP); it must not be a stale
 * TCP handle. */
h2_pal_result_t h2_bk_tls_tcp_close(h2_bk_tls_lifetime_t* manager, int fd);
/* Fail closed for unknown/logically closed/failed/canceled/expired records.
 * Call only while the native operation owns its lifetime ref. */
bool h2_bk_tls_bio_canceled(h2_bk_tls_lifetime_t* manager, int fd);

/* One absolute deadline includes validation, bounded copies, queueing, wait and
 * final publication. No allocations, native setup/destruction, joins or logs on
 * this path. Failure always preserves caller ownership; after accepted work it
 * leaves a tombstone until caller close. Success alone publishes fd. No worker
 * retains caller config/output pointers. Roots larger than inline storage use
 * staged worker allocation and caller copying under the same deadline, without
 * a replacement bundle limit. Missing large-root allocation ops or unavailable
 * workers return UNSUPPORTED; allocation failure returns NO_MEMORY; exhausted
 * jobs return NO_SPACE.
 * timeout_ms == 0 is INVALID_ARG, matching BK compatibility TLS validation. */
h2_pal_result_t h2_bk_tls_wrap_bounded(h2_bk_tls_lifetime_t* manager, int fd,
                                       const h2_pal_net_tls_config_t* config,
                                       uint32_t timeout_ms,
                                       h2_pal_net_socket_t* out_socket);

/* Perform at most one root allocation/free, queued wrap, context disposal or
 * physical close. Returns true when work progressed. Drain until false, then
 * use worker_wait_ms to park. Failed/canceled jobs and active TLS keep their
 * slot until caller close. Dynamic roots may be freed after wrap and waiter
 * finish, including ACTIVE jobs; the fd stays pinned during free. */
bool h2_bk_tls_worker_step(h2_bk_tls_lifetime_t* manager);
/* Next COPY_READY or unclaimed DONE/waiter deadline, or UINT32_MAX if no timed
 * recheck is needed. Zero means recheck now. Native semaphore waits may use
 * forever only for UINT32_MAX; all other values are finite. wake_worker must
 * preserve wake tokens across the drain/wait race and wake an idle worker for
 * each new job. Shutdown wakes every prestarted worker; this coordinator does
 * not join them.
 */
uint32_t h2_bk_tls_worker_wait_ms(h2_bk_tls_lifetime_t* manager);

#ifdef __cplusplus
}
#endif

#endif
