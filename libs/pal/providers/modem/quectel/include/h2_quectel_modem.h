#ifndef H2_QUECTEL_MODEM_H
#define H2_QUECTEL_MODEM_H

#include "h2/pal/hal/h2_pal_modem.h"
#include "h2_modem_urc.h"
#include "h2_modem_rx.h"
#include "h2/pal/os/h2_pal_sync.h"
#include "h2/pal/os/h2_pal_system_event.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_QUECTEL_LINE_MAX 192u
#define H2_QUECTEL_RESPONSE_MAX 4u
/* QuecLocator accepts at most 127 bytes of token. */
#define H2_QUECTEL_CELL_LOCATE_TOKEN_MAX 127u
#define H2_QUECTEL_CELL_LOCATE_TIMEOUT_MS 60000u

typedef struct h2_quectel_modem h2_quectel_modem_t;

typedef h2_pal_result_t (*h2_quectel_modem_init_fn)(void *user);
typedef h2_pal_result_t (*h2_quectel_modem_deinit_fn)(void *user);
typedef h2_pal_result_t (*h2_quectel_modem_flush_fn)(void *user);
typedef h2_pal_result_t (*h2_quectel_modem_read_fn)(
    void *user,
    uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *out_len);
typedef h2_pal_result_t (*h2_quectel_modem_write_fn)(
    void *user,
    const uint8_t *buf,
    size_t len,
    uint32_t timeout_ms,
    size_t *out_len);
typedef h2_pal_result_t (*h2_quectel_modem_command_fn)(
    void *user,
    const char *cmd,
    char *response,
    size_t response_size,
    uint32_t timeout_ms);

typedef enum h2_quectel_modem_profile {
    H2_QUECTEL_MODEM_PROFILE_UNSPECIFIED = 0,
    H2_QUECTEL_MODEM_PROFILE_EC25_UART = 1,
} h2_quectel_modem_profile_t;

/** @brief Board sleep gate, called under provider lock in task context.
 * false: drive DTR low and wait until UART/CMUX command transport is usable.
 * true: drain all TX/DLCIs, arm retained RI wake and lossless URC reception,
 * then drive DTR high. Must preserve network registration, SIM and modem VBAT.
 * Board owns electrical levels, USB suspend/disconnect, WAKEUP_IN/AP_READY and
 * firmware-specific settling time; return failure if any condition is unmet.
 * Must not call modem APIs or wait for a worker that calls modem APIs.
 * Success permits sleep, never proves it. Failure may leave hardware unknown.
 */
typedef h2_pal_result_t (*h2_quectel_modem_sleep_gate_fn)(void *user, int allow_sleep);

/** @brief Invalidate host PPP/netif immediately without AT I/O or reentry.
 * Called on SIM loss/reset under provider lock; cancel pending PPP callbacks,
 * discard old IP/DNS and queue teardown. Must not publish stale link-up later.
 */
typedef void (*h2_quectel_modem_invalidate_data_fn)(void *user);

/** @brief Restart the module and restore an AT-ready command transport.
 * Receives transport_user. Called in task context with operation_lock held and
 * the state mutex released. The integrator owns reset/power sequencing, bounded
 * readiness waits and draining startup RX/URCs before returning H2_PAL_OK.
 * Must not reenter command/lifecycle APIs or wait for tasks needing
 * operation_lock. RX/URC delivery remains allowed while the state lock is free.
 * Failure propagates to the caller; at most one attempt per provider instance.
 */
typedef h2_pal_result_t (*h2_quectel_modem_restart_module_fn)(void *user);

/* URC worker idle cadence, also used by deferred SIM recovery. */
#ifndef H2_QUECTEL_RING_POLL_INTERVAL_MS
#define H2_QUECTEL_RING_POLL_INTERVAL_MS 1000u
#endif
#ifndef H2_QUECTEL_RING_POLL_TIMEOUT_MS
#define H2_QUECTEL_RING_POLL_TIMEOUT_MS 1000u
#endif

typedef struct h2_quectel_modem_config {
    void *transport_user;
    h2_quectel_modem_init_fn init;
    h2_quectel_modem_deinit_fn deinit;
    h2_quectel_modem_flush_fn flush;
    h2_quectel_modem_read_fn read;
    h2_quectel_modem_write_fn write;
    h2_quectel_modem_command_fn command;
    const h2_pal_sync_api_t *sync_api;
    /* Supply both APIs for asynchronous RX. The worker lives from init to
     * deinit; sync_api must implement try_lock_mutex for deferred SIM recovery
     * and incoming-call CLCC watchdog,
     * and protects state independently of serialized AT operations.
     * With this worker, command() returns solicited text; any URCs included
     * in that text are ignored because physical RX already delivered them. */
    const h2_pal_task_api_t *urc_task_api;
    const h2_pal_queue_api_t *urc_queue_api;
    const h2_pal_mem_api_t *allocator;
    const h2_pal_system_event_api_t *system_events;
    uint32_t capabilities;
    uint32_t command_timeout_ms;
    uint32_t io_timeout_ms;
    /* QuecLocator identity token supplied by the integrator. Borrowed: the
     * string must outlive the modem instance. NULL or empty disables cell
     * locate entirely, so no token ever reaches the modem. The provider never
     * copies, logs or reports the value. */
    const char *cell_locate_token;
    /* Timeout for one cell locate round trip, 0 selects
     * H2_QUECTEL_CELL_LOCATE_TIMEOUT_MS. */
    uint32_t cell_locate_timeout_ms;
    /** Explicit module family; never inferred from generic capabilities. */
    h2_quectel_modem_profile_t profile;
    /** Required with sync_api and independent command channel for low power. */
    h2_quectel_modem_sleep_gate_fn sleep_gate;
    /** Optional hot-plug request. Requires EC25 UART profile (EC25/EC800M
     * families), sync_api, command and invalidate_data; sleep_gate is optional.
     * SIM_DET must be wired at sim_insert_level (0 or 1). Changed QSIMDET uses
     * restart_module once, then replays prepare and verifies the target value.
     * Failed readback returns INVALID_STATE without another write/restart.
     * Without the callback, INVALID_STATE remains latched until an external
     * module restart and new provider instance. */
    uint8_t sim_hotplug;
    uint8_t sim_insert_level;
    h2_quectel_modem_invalidate_data_fn invalidate_data;
    /** Optional module restart after changed QSIMDET; NULL preserves the latch. */
    h2_quectel_modem_restart_module_fn restart_module;
} h2_quectel_modem_config_t;

struct h2_quectel_modem {
    h2_pal_modem_t platform;
    h2_quectel_modem_config_t config;
    /** State mutex; AT I/O releases it while retaining operation_lock. */
    h2_pal_mutex_t *lock;
    h2_pal_mutex_t *operation_lock;
    h2_pal_modem_status_t observed_status;
    h2_pal_modem_signal_t observed_signal;
    uint8_t registration_seen;
    uint8_t packet_seen;
    uint8_t signal_seen;
    uint32_t reset_generation;
    uint32_t registration_generation;
    uint32_t packet_generation;
    uint32_t event_drop_count;
    h2_modem_urc_worker_t urc_worker;
    uint32_t operation_depth;
    h2_pal_modem_power_policy_t power_policy;
    uint8_t power_configured;
    uint8_t power_fault;
    uint8_t sleep_allowed;
    uint8_t gnss_hold;
    uint8_t call_hold;
    uint8_t data_hold;
    uint8_t model_checked;
    uint8_t sim_restart_required;
    uint8_t sim_restart_attempted;
    uint8_t preparing;
    uint32_t sim_generation;
    /* RX-written CPIN outcome; access atomically as in modem/common counters. */
    uint32_t cpin_absent_seen;
    /* Deferred insertion recovery; protected by the provider state lock. */
    uint8_t sim_poll_remaining;
    uint8_t sim_refresh_pending;
    uint8_t sim_presence; /* 0 unknown, 1 inserted, 2 removed */
    uint8_t sim_seen;
    h2_pal_modem_sim_state_t sim_state;
    uint8_t prepared;
    uint8_t opened;
    uint8_t cell_locate_token_sent;
    uint32_t capabilities;
    /* Optional DSCI configuration is volatile; ERROR is latched per instance. */
    uint8_t dsci_unsupported;
    uint8_t dsci_voice_seen;
    /* Per incoming occurrence: polling stops after voice DSCI or answer/end.
     * Without task/queue APIs there is no autonomous watchdog. Busy operations
     * skip a tick; failed CLCC is unknown and never synthesizes an end. */
    uint8_t incoming_dsci_seen;
    uint8_t incoming_answered;
    int32_t incoming_modem_call_id;
    /* Conservative raw-I/O wait budget used only during watchdog exchanges. */
    uint32_t call_poll_io_budget;
    uint8_t call_poll_running;
    uint8_t call_status_seen;
    uint32_t call_generation;
    int32_t dsci_call_id;
    h2_pal_modem_call_status_t observed_call;
    int32_t incoming_call_id;
    int32_t next_incoming_call_id;
    h2_pal_modem_data_status_t data_status;
    char last_apn[H2_PAL_MODEM_APN_MAX];
    char last_username[H2_PAL_MODEM_APN_MAX];
    char last_password[H2_PAL_MODEM_APN_MAX];
};

h2_pal_result_t h2_quectel_modem_init(
    h2_quectel_modem_t *modem,
    const h2_quectel_modem_config_t *config);
/** Stop/join external API and RX callers before deinit. Closes the transport,
 * stops the URC worker outside the provider lock, then releases resources.
 * On failure retain the instance and retry; never free it before success. */
h2_pal_result_t h2_quectel_modem_deinit(h2_quectel_modem_t *modem);
h2_pal_modem_t *h2_quectel_modem_platform(h2_quectel_modem_t *modem);
h2_pal_result_t h2_quectel_modem_set_apn(
    h2_pal_modem_t *platform,
    const h2_pal_modem_apn_config_t *config);
h2_pal_result_t h2_quectel_modem_prepare(h2_quectel_modem_t *modem);
/** @brief Apply a complete, classified URC from a task, never ISR.
 * Takes only the short state lock, never the AT operation lock. Callbacks
 * (system event, sleep gate, invalidate_data) must not reenter modem APIs or
 * wait for a command/RX/URC task. Stop/join callers before deinit.
 * Voice ^DSCI shares incoming IDs with RING/CLIP and deduplicates CLCC/call
 * results. After a voice DSCI, unnumbered terminal results are ignored until
 * module reset; identified DSCI CALL_END owns remote termination.
 * Without sync_api all calls require external serialization.
 */
void h2_quectel_handle_urc_line(h2_quectel_modem_t *modem, const char *line);
/** @brief Copy a complete, already classified notification to the worker.
 * Requires urc_task_api/urc_queue_api. No modem lock or waiting for queue space.
 * Unknown/ordinary response lines are ignored. Ambiguous response prefixes
 * require command-aware classification through rx_feed before this entry.
 * FULL/TRUNCATED is a lost occurrence: count/report it, never replay a batch.
 */
h2_pal_result_t h2_quectel_post_urc_line(h2_quectel_modem_t *modem, const char *line);
/** @brief Frame and classify physical RX bytes before queuing notifications.
 * receiver is caller-owned, zero-initialized per ordered command channel; see
 * h2_modem_rx_feed for authoritative physical offset, replay and gap semantics.
 * command is borrowed during this call: the command owning these bytes, or
 * NULL while idle. Serialize command-context changes with RX. A retained tail
 * belongs to its original command; finish/discard it at a known channel reset
 * before changing context. Never infer stream offsets from content/pointers.
 * For esp_modem cumulative callbacks, obtain the original buffer base offset
 * or feed only data + consumed, len at the DTE process_line boundary. The
 * legacy (data,total_len) URC hook alone cannot identify buffer reset/replay.
 * CMUX channels require separate receivers; do not feed PPP binary frames.
 * Single producer, task context only; no modem lock, allocation or blocking.
 * All bytes are consumed on FULL/TRUNCATED/FORMAT; return the first error.
 * Identical ambiguous query/URC formats (CPIN/CSQ/CGATT/CLCC/QSIMSTAT) during
 * their matching command are treated as solicited; transport must route any
 * independently identified notification via post_urc_line instead.
 * Exact SIM-absent CME answers during AT+CPIN? atomically mark that exchange;
 * they are not queued. The exchange applies ABSENT before returning failure.
 */
h2_pal_result_t h2_quectel_rx_feed(
    h2_quectel_modem_t *modem,
    h2_modem_rx_t *receiver,
    uint64_t offset,
    const uint8_t *data,
    size_t length,
    const char *command);
h2_pal_result_t h2_quectel_modem_dial_ppp(h2_quectel_modem_t *modem);
h2_pal_result_t h2_quectel_modem_drop_ppp(h2_quectel_modem_t *modem);

#ifdef __cplusplus
}
#endif

#endif
