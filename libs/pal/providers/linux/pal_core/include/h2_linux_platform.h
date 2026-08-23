#ifndef H2_LINUX_PLATFORM_H
#define H2_LINUX_PLATFORM_H

#include "h2_pal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct h2_linux_display_config {
    /** Borrowed framebuffer path retained by the process-wide provider. */
    const char *device_path;
    /** Required logical RGB565 viewport width. */
    uint32_t width;
    /** Required logical RGB565 viewport height. */
    uint32_t height;
} h2_linux_display_config_t;

#define H2_LINUX_EVDEV_DEVICE_NAME_MAX_LEN 80u
#define H2_LINUX_EVDEV_BUTTON_MAX 16u
#define H2_LINUX_GPIO_CHIP_LABEL_MAX_LEN 32u
#define H2_LINUX_GPIO_BUTTON_MAX 16u

typedef struct h2_linux_evdev_button_config {
    /** Board-owned physical peripheral identifier exposed by the provider. */
    h2_pal_periph_id_t periph_id;
    /** Stable EVIOCGNAME value; the provider does not retain this pointer. */
    const char *device_name;
    /** Linux input-event key code to sample from the matching device. */
    uint16_t key_code;
} h2_linux_evdev_button_config_t;

typedef struct h2_linux_gpio_button_config {
    /** Board-owned physical peripheral identifier exposed by the provider. */
    h2_pal_periph_id_t periph_id;
    /** Stable GPIO_GET_CHIPINFO_IOCTL label; not retained by the provider. */
    const char *chip_label;
    /** Zero-based line offset within the matching GPIO chip. */
    uint32_t line_offset;
    /** Non-zero when the electrically low level represents pressed. */
    int active_low;
} h2_linux_gpio_button_config_t;

typedef struct h2_linux_evdev_touch_config {
    /** Stable EVIOCGNAME value; the provider copies this string. */
    const char *device_name;
    /** Logical dimensions used before the configured orientation transform. */
    uint32_t width;
    uint32_t height;
    /** Apply swap first, then inversion in the oriented output dimensions. */
    int swap_xy;
    int invert_x;
    int invert_y;
} h2_linux_evdev_touch_config_t;

typedef struct h2_linux_host_fs h2_linux_host_fs_t;

int h2_linux_host_fs_create(const char *const *sources,
                            const char *const *targets, size_t mount_count,
                            h2_linux_host_fs_t **out_fs);
void h2_linux_host_fs_destroy(h2_linux_host_fs_t *fs);
const h2_pal_fs_api_t *h2_linux_host_fs_api(h2_linux_host_fs_t *fs);
const h2_pal_net_api_t *h2_linux_net_api(void);
int h2_linux_entropy(void *user, uint8_t *out, size_t len);

/** @brief Configure the process-wide framebuffer provider before display open. */
h2_pal_result_t h2_linux_configure_display(
    const h2_linux_display_config_t *config);
/** @brief Return the process-wide libc memory provider. */
const h2_pal_mem_api_t *h2_linux_mem_api(void);
/** @brief Return the process-wide stderr log provider. */
const h2_pal_log_api_t *h2_linux_log_api(void);
/** @brief Return the process-wide POSIX clock provider. */
const h2_pal_time_api_t *h2_linux_time_api(void);
/** @brief Return the process-wide pthread task provider. */
const h2_pal_task_api_t *h2_linux_task_api(void);
/** @brief Return the process-wide pthread queue provider. */
const h2_pal_queue_api_t *h2_linux_queue_api(void);
/** @brief Return the pthread mutex, semaphore, and condition provider. */
const h2_pal_sync_api_t *h2_linux_sync_api(void);
/** @brief Return the real Linux network-interface snapshot provider. */
const h2_pal_netif_api_t *h2_linux_netif_api(void);
/** @brief Return the Linux system-event provider with rtnetlink monitoring. */
const h2_pal_system_event_api_t *h2_linux_system_event_api(void);
#if defined(H2_LINUX_TESTING)
void h2_linux_netif_test_set_snapshot(
    const h2_pal_netif_status_t *entries,
    size_t count,
    const h2_pal_netif_dns_server_t *dns,
    size_t dns_count);
void h2_linux_netif_test_set_default(
    const h2_pal_netif_ref_t *ref,
    int valid);
h2_pal_result_t h2_linux_netif_test_reconcile_default(
    const h2_pal_netif_ref_t *ref,
    int valid);
#endif
/** @brief Return the process-wide Linux framebuffer display provider. */
const h2_pal_display_api_t *h2_linux_display_api(void);
/**
 * @brief Configure process-wide evdev single-button mappings.
 *
 * The provider copies all entries and device names synchronously. Configuration
 * must finish before Runtime input polling starts and must not race with reads.
 *
 * @param entries Mapping array, or NULL when count is zero.
 * @param count Number of mappings; must not exceed H2_LINUX_EVDEV_BUTTON_MAX.
 * @return H2_PAL_OK on success, otherwise H2_PAL_ERR_INVALID_ARG.
 */
h2_pal_result_t h2_linux_configure_evdev_buttons(
    const h2_linux_evdev_button_config_t *entries,
    size_t count);
/** @brief Return the process-wide Linux evdev single-button provider. */
const h2_pal_button_api_t *h2_linux_evdev_button_api(void);
/**
 * @brief Configure process-wide GPIO character-device button mappings.
 *
 * The provider copies all entries and chip labels synchronously. Configuration
 * must finish before Runtime input polling starts and must not race with reads.
 * GPIO chips are discovered from all numeric /dev/gpiochip* nodes and lines are
 * requested lazily as inputs on first read. Handles remain retained until a
 * read failure, reconfiguration, or process exit. No matching chip/line reports
 * H2_PAL_ERR_NOT_FOUND; duplicate matching labels report
 * H2_PAL_ERR_INVALID_STATE; permission failures report H2_PAL_ERR_UNAVAILABLE;
 * and a line owned by another consumer reports H2_PAL_ERR_BUSY. After device
 * removal, the failed handle is released and the next read discovers again.
 *
 * @param entries Mapping array, or NULL when count is zero.
 * @param count Number of mappings; must not exceed H2_LINUX_GPIO_BUTTON_MAX.
 * @return H2_PAL_OK on success, otherwise H2_PAL_ERR_INVALID_ARG or
 * H2_PAL_ERR_IO when prior line handles cannot be closed.
 */
h2_pal_result_t h2_linux_configure_gpio_buttons(
    const h2_linux_gpio_button_config_t *entries,
    size_t count);
/** @brief Return the process-wide Linux GPIO single-button provider. */
const h2_pal_button_api_t *h2_linux_gpio_button_api(void);

/**
 * @brief Configure the process-wide Linux evdev single-contact touch provider.
 *
 * The provider enumerates every numeric /dev/input/eventN node and discovers a
 * device by its stable EVIOCGNAME value. At open it requires valid
 * ABS_MT_POSITION_X/Y ranges from EVIOCGABS; a missing or degenerate axis
 * returns H2_PAL_ERR_UNSUPPORTED. Each raw axis is clamped to [minimum,
 * maximum] and mapped with integer floor division to [0, logical_size - 1]:
 * (clamped - minimum) * (logical_size - 1) / (maximum - minimum). The provider
 * then applies swap_xy, invert_x, and invert_y in that order. Configuration
 * must finish before the Touch PAL is opened and must not race with reads.
 */
h2_pal_result_t h2_linux_configure_evdev_touch(
    const h2_linux_evdev_touch_config_t *config);
/** @brief Return the process-wide Linux evdev Touch PAL provider. */
const h2_pal_touch_api_t *h2_linux_evdev_touch_api(void);

#if defined(H2_LINUX_TESTING)
typedef h2_pal_result_t (*h2_linux_evdev_test_read_fn)(
    void *user,
    const char *device_name,
    uint16_t key_code,
    h2_pal_button_state_t *out_state);
void h2_linux_evdev_test_set_reader(
    h2_linux_evdev_test_read_fn read_fn,
    void *user);
typedef h2_pal_result_t (*h2_linux_gpio_test_read_fn)(
    void *user,
    const char *chip_label,
    uint32_t line_offset,
    int active_low,
    h2_pal_button_state_t *out_state);
void h2_linux_gpio_test_set_reader(
    h2_linux_gpio_test_read_fn read_fn,
    void *user);
h2_pal_result_t h2_linux_gpio_test_discovery_result(
    int has_match,
    h2_pal_result_t candidate_error);
int h2_linux_gpio_test_is_chip_node_name(const char *name);
int h2_linux_evdev_touch_test_is_event_node_name(const char *name);
h2_pal_result_t h2_linux_evdev_touch_test_set_axes(
    int32_t x_minimum,
    int32_t x_maximum,
    int32_t y_minimum,
    int32_t y_maximum);
h2_pal_result_t h2_linux_evdev_touch_test_feed(
    uint16_t type,
    uint16_t code,
    int32_t value,
    h2_pal_touch_event_t *out_event);
#endif

#ifdef __cplusplus
}
#endif

#endif
