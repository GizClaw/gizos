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
     * deinit; sync_api protects state independently of serialized AT operations.
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
    /** Optional hot-plug request. Requires EC25 profile, sync_api, command and
     * invalidate_data. SIM_DET must be wired at sim_insert_level (0 or 1).
     * Changed QSIMDET requires an external module restart and new instance;
     * open returns INVALID_STATE until that lifecycle is completed. */
    uint8_t sim_hotplug;
    uint8_t sim_insert_level;
    h2_quectel_modem_invalidate_data_fn invalidate_data;
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
    uint32_t sim_generation;
    uint8_t sim_seen;
    h2_pal_modem_sim_state_t sim_state;
    uint8_t prepared;
    uint8_t opened;
    uint8_t cell_locate_token_sent;
    uint32_t capabilities;
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
