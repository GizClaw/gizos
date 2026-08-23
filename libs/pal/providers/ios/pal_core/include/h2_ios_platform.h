#ifndef H2_IOS_PLATFORM_H
#define H2_IOS_PLATFORM_H

#if defined(__OBJC__)
#import <UIKit/UIKit.h>
#else
typedef struct UIView UIView;
#endif

#include "h2_pal.h"

/**
 * Opaque handle for the example-only iOS PAL subset.
 *
 * This component currently provides Memory, Time, Queue, Display, and a native
 * pointer bridge for smoke Apps. It is not a complete iOS PAL backend; all
 * other Runtime capabilities remain unsupported until implemented and
 * validated explicitly.
 */
typedef struct h2_ios_platform h2_ios_platform_t;

typedef struct h2_ios_platform_config {
  int32_t display_width;
  int32_t display_height;
} h2_ios_platform_config_t;

/**
 * @brief Create the example iOS PAL subset and its UIKit surface.
 *
 * Call this function on the UIKit main thread. The returned platform owns its
 * surface until h2_ios_platform_destroy().
 */
h2_ios_platform_t *
h2_ios_platform_create(const h2_ios_platform_config_t *config);

/**
 * @brief Detach the UIKit surface and destroy the platform.
 *
 * The caller must first stop every Runtime/App operation that can use the
 * platform. Destruction synchronizes with the UIKit main queue, invalidates
 * the surface host, and removes the surface before releasing platform memory.
 */
void h2_ios_platform_destroy(h2_ios_platform_t *platform);

/**
 * @brief Return the borrowed UIKit surface owned by the platform.
 *
 * Access the returned view only on the UIKit main thread and do not retain it
 * beyond h2_ios_platform_destroy().
 */
UIView *h2_ios_platform_view(h2_ios_platform_t *platform);

const h2_pal_mem_api_t *h2_ios_platform_mem_api(void);
const h2_pal_time_api_t *h2_ios_platform_time_api(void);
const h2_pal_queue_api_t *h2_ios_platform_queue_api(void);
const h2_pal_display_api_t *
h2_ios_platform_display_api(h2_ios_platform_t *platform);

h2_pal_result_t h2_ios_platform_read_pointer(void *user, int32_t *out_x,
                                             int32_t *out_y, int *out_pressed);

const h2_pal_system_event_api_t *h2_ios_system_event_api(void);
h2_pal_ble_t *h2_ios_corebluetooth_ble(const h2_pal_mem_api_t *allocator);

#endif
