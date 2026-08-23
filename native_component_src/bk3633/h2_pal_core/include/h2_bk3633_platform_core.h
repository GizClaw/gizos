#ifndef H2_BK3633_PLATFORM_CORE_H
#define H2_BK3633_PLATFORM_CORE_H

#include "h2_bk3633_platform_entropy.h"
#include "h2_bm8563.h"
#include "h2/pal/hal/h2_pal_ble.h"
#include "h2/pal/hal/h2_pal_button.h"
#include "h2/pal/hal/h2_pal_buzzer.h"
#include "h2/pal/os/h2_pal_disk.h"
#include "h2/pal/os/h2_pal_firmware_info.h"
#include "h2/pal/hal/h2_pal_input.h"
#include "h2/pal/os/h2_pal_log.h"
#include "h2/pal/os/h2_pal_mem.h"
#include "h2/pal/hal/h2_pal_power.h"
#include "h2/pal/os/h2_pal_pref.h"
#include "h2/pal/os/h2_pal_queue.h"
#include "h2/pal/os/h2_pal_system_event.h"
#include "h2/pal/os/h2_pal_time.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H2_BK3633_SYSTEM_EVENT_PAYLOAD_MAX 32u
#define H2_BK3633_NVDS_APPLICATION_TAG_MIN 0xa0u
#define H2_BK3633_NVDS_APPLICATION_TAG_MAX 0xfeu
#define H2_BK3633_NVDS_VALUE_SIZE_MAX 255u
#define H2_BK3633_PWM_CHANNEL_COUNT 3u

typedef struct h2_bk3633_platform_button h2_bk3633_platform_button_t;
typedef struct h2_bk3633_platform_buzzer h2_bk3633_platform_buzzer_t;
typedef struct h2_bk3633_platform_battery h2_bk3633_platform_battery_t;
typedef struct h2_bk3633_platform_power h2_bk3633_platform_power_t;
typedef struct h2_libco h2_libco_t;

typedef enum h2_bk3633_platform_libco_result {
    H2_BK3633_PLATFORM_LIBCO_OK = 0,
    H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_ARG = -1,
    H2_BK3633_PLATFORM_LIBCO_ERR_INVALID_STATE = -2,
    H2_BK3633_PLATFORM_LIBCO_ERR_FULL = -3,
    H2_BK3633_PLATFORM_LIBCO_ERR_WAKE = -4,
    H2_BK3633_PLATFORM_LIBCO_ERR_NO_MEMORY = -5,
} h2_bk3633_platform_libco_result_t;

/** Image-owned inputs for the BK3633 completion bridge. */
typedef struct h2_bk3633_platform_libco_config {
    /** Borrowed cooperative executor used by every completion waiter. */
    h2_libco_t *executor;
    /** Borrowed allocator retained until unbind. */
    const h2_pal_mem_api_t *allocator;
    /** Maximum distinct pending or delivered completion keys. */
    size_t completion_capacity;
} h2_bk3633_platform_libco_config_t;

/** Configures BM8563-backed wall time while preserving RWIP monotonic time. */
typedef struct h2_bk3633_platform_time_config {
    h2_bm8563_config_t rtc;
} h2_bk3633_platform_time_config_t;

/** Image-composition resources required by the BK3633 BLE Host provider. */
typedef struct h2_bk3633_platform_ble_config {
    /** Borrowed allocator retained for the provider lifetime. */
    const h2_pal_mem_api_t *mem;
    const h2_pal_time_api_t *time;
    /** Maximum copied inbound GATT read/write requests awaiting dispatch. */
    size_t gatt_pending_access_capacity;
    uint32_t bootstrap_timeout_ms;
} h2_bk3633_platform_ble_config_t;

/** Configures one final-result battery source with cooperative ADC waiting. */
typedef struct h2_bk3633_platform_battery_config {
    h2_pal_periph_id_t periph_id;
    uint8_t adc_channel;
    uint8_t adc_mode;
    uint8_t adc_gpio_pin;
    uint8_t charging_gpio_pin;
    uint8_t complete_gpio_pin;
    uint8_t charging_active_level;
    uint8_t complete_active_level;
    uint16_t empty_raw;
    uint16_t full_raw;
    uint32_t sample_interval_ms;
    uint32_t conversion_timeout_ms;
    /** First GPIO sample is immediate; later charge edges use this debounce. */
    uint32_t charge_stable_ms;
} h2_bk3633_platform_battery_config_t;

typedef h2_pal_result_t (*h2_bk3633_platform_validate_wake_fn_t)(
    void *user, uint8_t gpio_pin);

/** Configures verified BK3633 sleep, deep-sleep and reboot operations. */
typedef struct h2_bk3633_platform_power_config {
    uint32_t boot_count;
    uint8_t deep_sleep_wake_gpio_pin;
    h2_bk3633_platform_validate_wake_fn_t validate_deep_sleep_wake;
    void *validate_user;
    const h2_pal_time_api_t *time;
    uint32_t readiness_timeout_ms;
} h2_bk3633_platform_power_config_t;

/** BK3633 GPIO input pull configuration. */
typedef enum h2_bk3633_platform_button_pull {
    H2_BK3633_PLATFORM_BUTTON_PULL_DOWN = 0,
    H2_BK3633_PLATFORM_BUTTON_PULL_UP,
    H2_BK3633_PLATFORM_BUTTON_PULL_NONE,
} h2_bk3633_platform_button_pull_t;

/** Configures one independently debounced caller-context GPIO button. */
typedef struct h2_bk3633_platform_button_config {
    h2_pal_periph_id_t periph_id;
    uint8_t gpio_pin;
    uint8_t active_level;
    h2_bk3633_platform_button_pull_t pull;
    uint32_t debounce_ms;
} h2_bk3633_platform_button_config_t;

/** Selects one channel from either BK3633 PWM block. */
typedef struct h2_bk3633_platform_pwm_channel {
    uint8_t block;
    uint8_t channel;
    uint8_t gpio_pin;
    bool continuous_mode; /**< Composition-selected SDK counter mode. */
} h2_bk3633_platform_pwm_channel_t;

/** Configures one dual-PWM continuous-tone Buzzer PAL provider. */
typedef struct h2_bk3633_platform_buzzer_config {
    h2_pal_periph_id_t periph_id;
    h2_bk3633_platform_pwm_channel_t frequency;
    h2_bk3633_platform_pwm_channel_t volume;
    uint32_t min_frequency_hz;
    uint32_t max_frequency_hz;
    uint32_t volume_pwm_frequency_hz;
    uint8_t volume_inverted;
} h2_bk3633_platform_buzzer_config_t;

/** Maps one portable Preference key to an application-owned NVDS tag. */
typedef struct h2_bk3633_nvds_pref_entry {
    const char *name_space; /**< Portable Preference namespace. */
    const char *key; /**< Portable Preference key. */
    h2_pal_pref_entry_type_t type; /**< Declared value type. */
    uint8_t nvds_tag; /**< Unique NVDS application tag. */
    size_t max_value_size; /**< Maximum encoded byte count. */
} h2_bk3633_nvds_pref_entry_t;

/** Describes one board-owned raw-flash partition. */
typedef struct h2_bk3633_flash_partition_config {
    uint32_t id; /**< Stable portable partition identifier. */
    const char *name; /**< Stable portable partition name. */
    uint32_t offset; /**< Absolute flash byte offset. */
    uint32_t size; /**< Partition size in bytes. */
    uint32_t erase_block_size; /**< Required erase alignment in bytes. */
    uint32_t write_alignment; /**< Required write alignment in bytes. */
    uint32_t flags; /**< H2_PAL_DISK_PARTITION_FLAG_* mask. */
} h2_bk3633_flash_partition_config_t;

typedef size_t (*h2_bk3633_platform_log_sink_write_fn_t)(void *user,
                                                         const uint8_t *data,
                                                         size_t length);

typedef struct h2_bk3633_platform_log_config {
    h2_bk3633_platform_log_sink_write_fn_t write;
    void *user;
    size_t max_write_size;
} h2_bk3633_platform_log_config_t;

/** Caller-owned image-lifetime storage for the BK3633 Memory provider. */
typedef struct h2_bk3633_platform_mem_config {
    void *storage;
    size_t storage_size;
} h2_bk3633_platform_mem_config_t;

/** Snapshot of the configured BK3633 Memory arena. */
typedef struct h2_bk3633_platform_mem_stats {
    size_t capacity;
    size_t used_bytes;
    size_t free_bytes;
    size_t largest_free_block;
    size_t failed_allocations;
    size_t last_failed_request;
} h2_bk3633_platform_mem_stats_t;

/* This header exposes only adapters implemented in this component. BLE scan
 * and legacy advertising operations submit asynchronous SDK messages; the
 * project TASK_APP dispatcher must forward its GAPM messages to dispatch(). */
/**
 * Configures the singleton BLE Host provider once for the image lifetime.
 *
 * Scalar values are copied. The Memory and Time PAL objects are borrowed for
 * the provider lifetime. The provider owns queue storage allocated through
 * Memory PAL.
 */
h2_pal_result_t h2_bk3633_platform_ble_configure(
    const h2_bk3633_platform_ble_config_t *config);
const h2_pal_ble_host_api_t *h2_bk3633_platform_ble_api(void);
int h2_bk3633_platform_ble_dispatch(uint16_t msgid,
                                    const void *param,
                                    uint16_t dest_id,
                                    uint16_t src_id);

/*
 * The image-owned TASK_APP bootstrap reports its lifecycle through these
 * target-private hooks. Operations are unavailable until host_status() is OK.
 */
h2_pal_result_t h2_bk3633_platform_ble_host_bootstrap_begin(void);
void h2_bk3633_platform_ble_host_bootstrap_complete(h2_pal_result_t result);
h2_pal_result_t h2_bk3633_platform_ble_host_status(void);

/** Drain bounded copied BLE callbacks and events after a RWIP task turn. */
h2_pal_result_t h2_bk3633_platform_ble_dispatch_pending(void);

/** Return metadata embedded in the currently running BK3633 firmware. */
const h2_pal_firmware_info_api_t *h2_bk3633_platform_firmware_info_api(void);

/**
 * Return the BK3633 platform system-event backend.
 *
 * post() copies events into component-owned storage. The BLE Stack task calls
 * h2_bk3633_platform_system_event_dispatch_pending() only after raw
 * rwip_schedule() returns, so Runtime mapping does not run on the SDK's deep
 * scheduler stack.
 */
const h2_pal_system_event_api_t *h2_bk3633_platform_system_event_api(void);
h2_pal_result_t h2_bk3633_platform_system_event_dispatch_pending(void);

/**
 * Dispatch at most one copied system event outside rwip_schedule().
 *
 * If the system-event queue is empty, this operation first advances bounded
 * BLE provider work and then dispatches at most one event produced by it.
 * Repeated calls preserve FIFO order and keep the Runtime/App handoff bounded.
 */
h2_pal_result_t h2_bk3633_platform_system_event_dispatch_next(
    bool *out_more_work);

/**
 * Bind the image-owned cooperative executor at the safe root boundary.
 *
 * The configuration is copied. Completion metadata is allocated once through
 * @p config->allocator and freed by h2_bk3633_platform_libco_unbind().
 */
h2_bk3633_platform_libco_result_t
h2_bk3633_platform_libco_bind(
    const h2_bk3633_platform_libco_config_t *config);

/** Drop pending completion records and release the borrowed executor. */
void h2_bk3633_platform_libco_unbind(void);

/**
 * Record one owner-defined completion key without switching context.
 *
 * This bounded operation is safe for BK3633 IRQ/RWIP-depth producers. A key
 * already pending or delivered to its single waiter is coalesced. The root
 * loop must later call h2_bk3633_platform_libco_dispatch_wakes().
 */
h2_bk3633_platform_libco_result_t
h2_bk3633_platform_libco_record_completion(uintptr_t wait_key);

/** Deliver at most @p work_budget recorded completion keys from the root. */
h2_bk3633_platform_libco_result_t
h2_bk3633_platform_libco_dispatch_wakes(size_t work_budget,
                                        size_t *out_dispatched);
bool h2_bk3633_platform_libco_has_pending(void);

/**
 * Suspend the current cooperative task on one target-private completion.
 *
 * A completion key belongs to at most one current waiter. Real completion
 * state and payload remain owned by the producer and must be rechecked after
 * this operation returns.
 */
h2_pal_result_t h2_bk3633_platform_libco_wait(uintptr_t wait_key,
                                              uint32_t timeout_ms);

/**
 * Configure the board-owned output sink used by the BK3633 log backend.
 *
 * The component copies @p config. Each drain call invokes the sink at most
 * once and never requests more than @c max_write_size bytes.
 */
h2_pal_result_t
h2_bk3633_platform_log_init(const h2_bk3633_platform_log_config_t *config);
void h2_bk3633_platform_log_deinit(void);
const h2_pal_log_api_t *h2_bk3633_platform_log_api(void);

/**
 * Initialize the single BK3633 Memory provider over caller-owned storage.
 *
 * The storage must remain valid for the image lifetime and its address must
 * satisfy max_align_t alignment. Its size is rounded down to that alignment.
 * Initialization is allowed exactly once and must precede every allocation.
 */
h2_pal_result_t h2_bk3633_platform_mem_init(
    const h2_bk3633_platform_mem_config_t *config);

/** Borrow the image-lifetime BK3633 Memory PAL object. */
const h2_pal_mem_api_t *h2_bk3633_platform_mem_api(void);

/** Read a synchronous snapshot of arena usage and allocation failures. */
h2_pal_result_t h2_bk3633_platform_mem_get_stats(
    h2_bk3633_platform_mem_stats_t *out_stats);

/** Bind the single chip Time provider to a board-owned RTC transport. */
h2_pal_result_t
h2_bk3633_platform_time_init(const h2_bk3633_platform_time_config_t *config);

/** Release RTC state; monotonic time remains available. */
void h2_bk3633_platform_time_deinit(void);

const h2_pal_time_api_t *h2_bk3633_platform_time_api(void);

/**
 * Create the single bounded Battery Input provider for this chip.
 *
 * read_battery() returns one fresh/cached reading or a terminal timeout,
 * cancellation, or I/O error. A pending ADC conversion suspends the calling
 * cooperative task internally. Config is copied; Memory and Time APIs are
 * borrowed until deinit. A second live instance returns INVALID_STATE because
 * the provider exclusively arbitrates the chip ADC transaction.
 */
h2_pal_result_t h2_bk3633_platform_battery_init(
    const h2_bk3633_platform_battery_config_t *config,
    const h2_pal_mem_api_t *mem,
    const h2_pal_time_api_t *time,
    h2_bk3633_platform_battery_t **out_battery);

const h2_pal_input_api_t *
h2_bk3633_platform_battery_api(h2_bk3633_platform_battery_t *battery);

/** Stop any pending conversion and park the ADC input low. */
h2_pal_result_t
h2_bk3633_platform_battery_prepare_sleep(h2_bk3633_platform_battery_t *battery);

/** Restore GPIO sampling and ADC configuration after standby wake. */
h2_pal_result_t
h2_bk3633_platform_battery_restore(h2_bk3633_platform_battery_t *battery);

void h2_bk3633_platform_battery_deinit(h2_bk3633_platform_battery_t *battery);

h2_pal_result_t
h2_bk3633_platform_power_init(const h2_bk3633_platform_power_config_t *config,
                              const h2_pal_mem_api_t *mem,
                              h2_bk3633_platform_power_t **out_power);

const h2_pal_power_api_t *
h2_bk3633_platform_power_api(h2_bk3633_platform_power_t *power);

void h2_bk3633_platform_power_deinit(h2_bk3633_platform_power_t *power);

/**
 * Create a reusable GPIO Button PAL provider.
 *
 * Config entries are copied. The Memory and Time APIs are borrowed until
 * deinit. Each read samples GPIO and advances only that button's debounce
 * candidate in caller context; no task, timer or callback is created.
 */
h2_pal_result_t h2_bk3633_platform_button_init(
    const h2_bk3633_platform_button_config_t *configs,
    size_t config_count,
    const h2_pal_mem_api_t *mem,
    const h2_pal_time_api_t *time,
    h2_bk3633_platform_button_t **out_button);

/** Return the provider-owned Button API borrowed until deinit. */
const h2_pal_button_api_t *
h2_bk3633_platform_button_api(h2_bk3633_platform_button_t *button);

/** Release a partially or fully initialized Button provider. */
void h2_bk3633_platform_button_deinit(h2_bk3633_platform_button_t *button);

/**
 * Create a reusable dual-PWM continuous-tone Buzzer PAL provider.
 *
 * The provider copies config and initializes each configured PWM channel once.
 * Subsequent start and stop operations only update period and duty registers.
 */
h2_pal_result_t
h2_bk3633_platform_buzzer_init(const h2_bk3633_platform_buzzer_config_t *config,
                               const h2_pal_mem_api_t *mem,
                               h2_bk3633_platform_buzzer_t **out_buzzer);

/** Return the provider-owned Buzzer API borrowed until deinit. */
const h2_pal_buzzer_api_t *
h2_bk3633_platform_buzzer_api(h2_bk3633_platform_buzzer_t *buzzer);

/** Silence both PWM channels and release provider-owned state. */
void h2_bk3633_platform_buzzer_deinit(h2_bk3633_platform_buzzer_t *buzzer);

/**
 * Initializes the NVDS-backed Preference PAL after SDK NVDS initialization.
 *
 * The mapping array and all referenced strings are borrowed until deinit.
 * Tags must be unique and inside the application-owned range. Strings are
 * stored as UTF-8 bytes without their terminating NUL; get_string allocates
 * and appends the caller-visible terminator. Scalar values use little-endian
 * bytes, and writes persist immediately. Preference commit is a successful
 * no-op and does not provide transaction atomicity.
 */
h2_pal_result_t
h2_bk3633_nvds_pref_init(const h2_bk3633_nvds_pref_entry_t *entries,
                         size_t entry_count);

/** Releases all Preference handles and returns the provider to unsupported. */
void h2_bk3633_nvds_pref_deinit(void);

/** Returns the ready Preference provider or the canonical unsupported API. */
const h2_pal_pref_api_t *h2_bk3633_nvds_pref_api(void);

/**
 * Initializes the raw-flash Disk PAL with board-owned partitions.
 *
 * The partition array and names are borrowed until deinit. Only declared
 * partitions are visible; portable callers address bytes relative to them.
 */
h2_pal_result_t
h2_bk3633_flash_disk_init(const h2_bk3633_flash_partition_config_t *partitions,
                          size_t partition_count);

/** Returns the Disk provider to unsupported. */
void h2_bk3633_flash_disk_deinit(void);

/** Returns the ready Disk provider or the canonical unsupported API. */
const h2_pal_disk_api_t *h2_bk3633_flash_disk_api(void);

/* Drain at most one board-configured bounded chunk from normal context. */
h2_pal_result_t h2_bk3633_platform_log_drain(void);

#ifdef __cplusplus
}
#endif

#endif
