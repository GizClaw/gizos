#ifndef H2_ANDROID_PLATFORM_H
#define H2_ANDROID_PLATFORM_H

#include "h2_pal.h"

#include <android/bitmap.h>
#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Opaque handle for the Android PAL provider.
 *
 * Core capabilities use native pthreads and clocks; the existing display,
 * input and media integrations remain platform-specific.
 */
typedef struct h2_android_platform h2_android_platform_t;

typedef struct h2_android_platform_config {
  int32_t display_width;
  int32_t display_height;
} h2_android_platform_config_t;

h2_android_platform_t *
h2_android_platform_create(JNIEnv *env, jobject view,
                           const h2_android_platform_config_t *config);
void h2_android_platform_destroy(h2_android_platform_t *platform);

const h2_pal_mem_api_t *h2_android_platform_mem_api(void);
const h2_pal_time_api_t *h2_android_platform_time_api(void);
const h2_pal_queue_api_t *h2_android_platform_queue_api(void);
const h2_pal_audio_api_t *
h2_android_platform_audio_api(h2_android_platform_t *platform);
const h2_pal_audio_decoder_api_t *
h2_android_platform_audio_decoder_api(void);
const h2_pal_video_decoder_api_t *
h2_android_platform_video_decoder_api(void);
const h2_pal_display_api_t *
h2_android_platform_display_api(h2_android_platform_t *platform);

h2_pal_result_t h2_android_platform_read_pointer(void *user, int32_t *out_x,
                                                 int32_t *out_y,
                                                 int *out_pressed);
void h2_android_platform_update_pointer(h2_android_platform_t *platform,
                                        int32_t x, int32_t y, int pressed);
int h2_android_platform_copy_frame(h2_android_platform_t *platform, JNIEnv *env,
                                   jobject bitmap);

/* Quiescent counters cover PAL-owned resources, not process RSS. */
typedef struct h2_android_platform_resource_stats {
    size_t tasks, task_stack_bytes, queues, mutexes, semaphores, conditions, timers;
    size_t allocations, allocation_bytes;
} h2_android_platform_resource_stats_t;
const h2_pal_task_api_t *h2_android_platform_task_api(void);
const h2_pal_sync_api_t *h2_android_platform_sync_api(void);
const h2_pal_timer_api_t *h2_android_platform_timer_api(void);
const h2_pal_log_api_t *h2_android_platform_log_api(void);
const h2_pal_firmware_info_api_t *h2_android_platform_firmware_info_api(void);
h2_pal_result_t h2_android_platform_get_resource_stats(h2_android_platform_resource_stats_t *out);
/* Fault injection applies only to task-stack allocation; -1 disables it.
 * Configure only from a quiescent diagnostic harness. */
h2_pal_result_t h2_android_platform_task_allocation_fault(int successful_before_failure);
/* Process-wide Core teardown: stop all callers and release handles first.
 * A busy Core is retained and may be retried after its owners have finished. */
h2_pal_result_t h2_android_platform_core_shutdown(void);
const h2_pal_system_event_api_t *h2_android_system_event_api(void);
/* Supply the host APK's PackageInfo.versionName before Runtime creation. */
h2_pal_result_t h2_android_platform_set_image_version(const char *version);

/** Storage owner for one sandbox directory. Paths under portable_root map to
 * directory/files; Preferences use directory/preferences.sqlite. The caller
 * owns the directory and must supply an absolute path with an existing parent.
 * NULL output on failure; config strings are copied. Close every borrowed
 * file, namespace and cursor before destroy. No existing directory is cleared.
 */
typedef struct h2_android_storage h2_android_storage_t;
h2_pal_result_t h2_android_storage_create(const char *directory,
    const char *portable_root, h2_android_storage_t **out);
const h2_pal_fs_api_t *h2_android_storage_fs_api(h2_android_storage_t *storage);
const h2_pal_pref_api_t *h2_android_storage_pref_api(h2_android_storage_t *storage);
void h2_android_storage_destroy(h2_android_storage_t *storage);

/** Return a borrowed process-wide Crypto provider, initialized on first use
 * with OS secure entropy. Initialization failure returns the canonical
 * unsupported API. Concurrent operations are supported; the owner must quiesce
 * all users before shutdown. The next getter may initialize a new lifetime. */
const h2_pal_crypto_api_t *h2_android_platform_crypto_api(void);
/** Retire the provider after every Runtime and Crypto operation has stopped.
 * Idempotent; also invoked by successful platform_core_shutdown(). */
void h2_android_platform_crypto_shutdown(void);

/** Create a separately owned JSON PAL provider from the package's yyjson
 * implementation. The borrowed API and Memory PAL backend must outlive the
 * provider. Destroy rejects live documents or serialized buffers. */
h2_pal_result_t h2_android_json_provider_create(
    const h2_pal_mem_api_t *mem, void **out_provider,
    const h2_pal_json_api_t **out_api);
h2_pal_result_t h2_android_json_provider_destroy(void **provider);

/** HTTP owner over native POSIX networking and the shared full WolfSSL provider.
 * create copies the optional root CA; NULL/zero selects system trust. Output is
 * NULL on failure. API remains borrowed until destroy. Close all responses and
 * let every synchronous request return before destroy or Core shutdown. The
 * owner holds its own TLS lifecycle reference, independent of Crypto callers.
 */
typedef struct h2_android_http h2_android_http_t;
h2_pal_result_t h2_android_http_create(const uint8_t *root_ca_pem,
    size_t root_ca_pem_len, h2_android_http_t **out);
const h2_pal_http_api_t *h2_android_http_api(h2_android_http_t *http);
void h2_android_http_destroy(h2_android_http_t *http);

/** Own a native H2Peer/H2SCTP WebRTC provider using OS networking and the
 * shared full WolfSSL DTLS/Crypto provider. Create clears output on failure.
 * Keep every borrowed API and owned event within this owner's lifetime; stop
 * calls and close peers before destroy. Destroy clears output on success and
 * preserves a busy owner for retry. No physical audio device is opened. */
typedef struct h2_android_webrtc h2_android_webrtc_t;
h2_pal_result_t h2_android_webrtc_create(h2_android_webrtc_t **out);
const h2_pal_webrtc_api_t *h2_android_webrtc_api(h2_android_webrtc_t *owner);
h2_pal_result_t h2_android_webrtc_destroy(h2_android_webrtc_t **owner);

#ifdef __cplusplus
}
#endif

#endif
