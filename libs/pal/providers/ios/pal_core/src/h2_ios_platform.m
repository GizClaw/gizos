#import "h2_ios_platform.h"

#import <CoreGraphics/CoreGraphics.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct h2_pal_queue {
  pthread_mutex_t mutex;
  pthread_cond_t not_empty;
  pthread_cond_t not_full;
  const h2_pal_mem_api_t *allocator;
  size_t item_size;
  size_t item_count;
  size_t head;
  size_t count;
  int closed;
  uint8_t *items;
};

@class H2IOSPlatformView;

struct h2_ios_platform {
  pthread_mutex_t mutex;
  uint32_t *rgba;
  int32_t width;
  int32_t height;
  int32_t pointer_x;
  int32_t pointer_y;
  int pointer_pressed;
  H2IOSPlatformView *view;
  h2_pal_display_api_t display;
  int opened;
};

@interface H2IOSPlatformView : UIView
- (instancetype)initWithHost:(h2_ios_platform_t *)host;
- (void)detachHost;
@end

static CGRect h2_ios_platform_content_rect(h2_ios_platform_t *host,
                                           CGRect bounds) {
  const CGFloat logical_width = host->width;
  const CGFloat logical_height = host->height;
  const CGFloat scale = MIN(bounds.size.width / logical_width,
                            bounds.size.height / logical_height);
  const CGSize size = CGSizeMake(logical_width * scale, logical_height * scale);
  return CGRectMake((bounds.size.width - size.width) * 0.5,
                    (bounds.size.height - size.height) * 0.5, size.width,
                    size.height);
}

@implementation H2IOSPlatformView {
  h2_ios_platform_t *_host;
}

- (instancetype)initWithHost:(h2_ios_platform_t *)host {
  self = [super initWithFrame:CGRectZero];
  if (self != nil) {
    _host = host;
    self.backgroundColor = [UIColor colorWithRed:0.02
                                           green:0.03
                                            blue:0.07
                                           alpha:1.0];
    self.multipleTouchEnabled = NO;
    self.contentMode = UIViewContentModeRedraw;
  }
  return self;
}

- (void)detachHost {
  _host = NULL;
}

- (void)drawRect:(CGRect)rect {
  (void)rect;
  h2_ios_platform_t *host = _host;
  if (host == NULL) {
    return;
  }
  const size_t byte_count =
      (size_t)host->width * host->height * sizeof(uint32_t);
  pthread_mutex_lock(&host->mutex);
  NSData *snapshot = host->rgba == NULL
                         ? nil
                         : [NSData dataWithBytes:host->rgba length:byte_count];
  pthread_mutex_unlock(&host->mutex);
  if (snapshot == nil) {
    return;
  }

  CGDataProviderRef provider =
      CGDataProviderCreateWithCFData((__bridge CFDataRef)snapshot);
  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  CGImageRef image = CGImageCreate(
      host->width, host->height, 8, 32,
      (size_t)host->width * sizeof(uint32_t), color_space,
      (CGBitmapInfo)((uint32_t)kCGBitmapByteOrder32Little |
                     (uint32_t)kCGImageAlphaPremultipliedFirst),
      provider,
      NULL, false, kCGRenderingIntentDefault);
  CGContextRef context = UIGraphicsGetCurrentContext();
  if (context != NULL && image != NULL) {
    CGContextSaveGState(context);
    CGContextTranslateCTM(context, 0.0, self.bounds.size.height);
    CGContextScaleCTM(context, 1.0, -1.0);
    CGRect target = h2_ios_platform_content_rect(host, self.bounds);
    target.origin.y = self.bounds.size.height - CGRectGetMaxY(target);
    CGContextSetInterpolationQuality(context, kCGInterpolationNone);
    CGContextDrawImage(context, target, image);
    CGContextRestoreGState(context);
  }
  if (image != NULL) {
    CGImageRelease(image);
  }
  CGColorSpaceRelease(color_space);
  CGDataProviderRelease(provider);
}

- (void)updateTouch:(UITouch *)touch pressed:(BOOL)pressed {
  h2_ios_platform_t *host = _host;
  if (host == NULL) {
    return;
  }
  const CGPoint point = [touch locationInView:self];
  const CGRect content = h2_ios_platform_content_rect(host, self.bounds);
  int32_t x = (int32_t)((point.x - content.origin.x) * host->width /
                        content.size.width);
  int32_t y = (int32_t)((point.y - content.origin.y) * host->height /
                        content.size.height);
  x = MAX(0, MIN(host->width - 1, x));
  y = MAX(0, MIN(host->height - 1, y));
  pthread_mutex_lock(&host->mutex);
  host->pointer_x = x;
  host->pointer_y = y;
  host->pointer_pressed = pressed ? 1 : 0;
  pthread_mutex_unlock(&host->mutex);
}

- (void)touchesBegan:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  (void)event;
  [self updateTouch:touches.anyObject pressed:YES];
}

- (void)touchesMoved:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  (void)event;
  [self updateTouch:touches.anyObject pressed:YES];
}

- (void)touchesEnded:(NSSet<UITouch *> *)touches withEvent:(UIEvent *)event {
  (void)event;
  [self updateTouch:touches.anyObject pressed:NO];
}

- (void)touchesCancelled:(NSSet<UITouch *> *)touches
               withEvent:(UIEvent *)event {
  (void)event;
  [self updateTouch:touches.anyObject pressed:NO];
}

@end

static void *h2_ios_platform_alloc(void *user, size_t len) {
  (void)user;
  return malloc(len);
}

static void *h2_ios_platform_realloc(void *user, void *pointer, size_t len) {
  (void)user;
  return realloc(pointer, len);
}

static void h2_ios_platform_free(void *user, void *pointer) {
  (void)user;
  free(pointer);
}

static const h2_pal_mem_vtable_t s_h2_ios_platform_mem_vtable = {
    .alloc = h2_ios_platform_alloc,
    .realloc = h2_ios_platform_realloc,
    .free = h2_ios_platform_free,
};
static const h2_pal_mem_api_t s_h2_ios_platform_mem = {
    .user = NULL,
    .vtable = &s_h2_ios_platform_mem_vtable,
};

static h2_pal_result_t h2_ios_platform_time_get_monotonic_ms(void *user,
                                                             uint64_t *out_ms) {
  (void)user;
  struct timespec value = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) {
    return H2_PAL_ERR_IO;
  }
  *out_ms = (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
  return H2_PAL_OK;
}

static h2_pal_result_t h2_ios_platform_time_sleep_ms(void *user, uint32_t ms) {
  (void)user;
  struct timespec remaining = {
      .tv_sec = (time_t)(ms / 1000u),
      .tv_nsec = (long)(ms % 1000u) * 1000000l,
  };
  while (nanosleep(&remaining, &remaining) != 0) {
    if (errno != EINTR) {
      return H2_PAL_ERR_IO;
    }
  }
  return H2_PAL_OK;
}

static const h2_pal_time_vtable_t s_h2_ios_platform_time_vtable = {
    .get_monotonic_ms = h2_ios_platform_time_get_monotonic_ms,
    .sleep_ms = h2_ios_platform_time_sleep_ms,
};
static const h2_pal_time_api_t s_h2_ios_platform_time = {
    .user = NULL,
    .vtable = &s_h2_ios_platform_time_vtable,
};

static int h2_ios_platform_queue_create(void *user,
                                        const h2_pal_queue_config_t *config,
                                        h2_pal_queue_t **out_queue) {
  (void)user;
  if (config == NULL || config->allocator == NULL || out_queue == NULL ||
      config->item_size == 0u || config->item_count == 0u ||
      config->item_size > SIZE_MAX / config->item_count) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  *out_queue = NULL;
  h2_pal_queue_t *queue = h2_pal_mem_alloc(config->allocator, sizeof(*queue));
  if (queue == NULL) {
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  memset(queue, 0, sizeof(*queue));
  queue->items = h2_pal_mem_alloc(config->allocator,
                                  config->item_size * config->item_count);
  if (queue->items == NULL) {
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  if (pthread_mutex_init(&queue->mutex, NULL) != 0) {
    h2_pal_mem_free(config->allocator, queue->items);
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  if (pthread_cond_init(&queue->not_empty, NULL) != 0) {
    (void)pthread_mutex_destroy(&queue->mutex);
    h2_pal_mem_free(config->allocator, queue->items);
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  if (pthread_cond_init(&queue->not_full, NULL) != 0) {
    (void)pthread_cond_destroy(&queue->not_empty);
    (void)pthread_mutex_destroy(&queue->mutex);
    h2_pal_mem_free(config->allocator, queue->items);
    h2_pal_mem_free(config->allocator, queue);
    return H2_PAL_QUEUE_ERR_NO_MEMORY;
  }
  queue->allocator = config->allocator;
  queue->item_size = config->item_size;
  queue->item_count = config->item_count;
  *out_queue = queue;
  return H2_PAL_QUEUE_OK;
}

static void h2_ios_platform_queue_destroy(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return;
  }
  (void)pthread_cond_destroy(&queue->not_empty);
  (void)pthread_cond_destroy(&queue->not_full);
  (void)pthread_mutex_destroy(&queue->mutex);
  h2_pal_mem_free(queue->allocator, queue->items);
  h2_pal_mem_free(queue->allocator, queue);
}

static int h2_ios_platform_queue_send(void *user, h2_pal_queue_t *queue,
                                      const void *item, uint32_t timeout_ms) {
  (void)user;
  if (queue == NULL || item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  if (queue->count == queue->item_count && !queue->closed && timeout_ms != 0u) {
    if (timeout_ms == UINT32_MAX) {
      while (queue->count == queue->item_count && !queue->closed) {
        (void)pthread_cond_wait(&queue->not_full, &queue->mutex);
      }
    } else {
      struct timespec deadline = {0};
      (void)clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += (time_t)(timeout_ms / 1000u);
      deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000l;
      if (deadline.tv_nsec >= 1000000000l) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000l;
      }
      while (queue->count == queue->item_count && !queue->closed) {
        if (pthread_cond_timedwait(&queue->not_full, &queue->mutex,
                                   &deadline) != 0) {
          break;
        }
      }
    }
  }
  if (queue->closed || queue->count == queue->item_count) {
    pthread_mutex_unlock(&queue->mutex);
    return queue->closed ? H2_PAL_QUEUE_ERR_CLOSED
                         : (timeout_ms == 0u ? H2_PAL_ERR_FULL
                                             : H2_PAL_QUEUE_ERR_TIMEOUT);
  }
  const size_t tail = (queue->head + queue->count) % queue->item_count;
  memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int h2_ios_platform_queue_send_latest(void *user, h2_pal_queue_t *queue,
                                             const void *item) {
  (void)user;
  if (queue == NULL || item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  if (queue->closed) {
    pthread_mutex_unlock(&queue->mutex);
    return H2_PAL_QUEUE_ERR_CLOSED;
  }
  if (queue->count == queue->item_count) {
    queue->head = (queue->head + 1u) % queue->item_count;
    --queue->count;
  }
  const size_t tail = (queue->head + queue->count) % queue->item_count;
  memcpy(queue->items + tail * queue->item_size, item, queue->item_size);
  ++queue->count;
  pthread_cond_signal(&queue->not_empty);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int h2_ios_platform_queue_recv(void *user, h2_pal_queue_t *queue,
                                      void *out_item, uint32_t timeout_ms) {
  (void)user;
  if (queue == NULL || out_item == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  if (queue->count == 0u && !queue->closed && timeout_ms != 0u) {
    if (timeout_ms == UINT32_MAX) {
      while (queue->count == 0u && !queue->closed) {
        (void)pthread_cond_wait(&queue->not_empty, &queue->mutex);
      }
    } else {
      struct timespec deadline = {0};
      (void)clock_gettime(CLOCK_REALTIME, &deadline);
      deadline.tv_sec += (time_t)(timeout_ms / 1000u);
      deadline.tv_nsec += (long)(timeout_ms % 1000u) * 1000000l;
      if (deadline.tv_nsec >= 1000000000l) {
        ++deadline.tv_sec;
        deadline.tv_nsec -= 1000000000l;
      }
      while (queue->count == 0u && !queue->closed) {
        if (pthread_cond_timedwait(&queue->not_empty, &queue->mutex,
                                   &deadline) != 0) {
          break;
        }
      }
    }
  }
  if (queue->count == 0u) {
    const int result = queue->closed
                           ? H2_PAL_QUEUE_ERR_CLOSED
                           : (timeout_ms == 0u ? H2_PAL_ERR_WOULD_BLOCK
                                               : H2_PAL_QUEUE_ERR_TIMEOUT);
    pthread_mutex_unlock(&queue->mutex);
    return result;
  }
  memcpy(out_item, queue->items + queue->head * queue->item_size,
         queue->item_size);
  queue->head = (queue->head + 1u) % queue->item_count;
  --queue->count;
  pthread_cond_signal(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int h2_ios_platform_queue_reset(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  queue->head = 0u;
  queue->count = 0u;
  pthread_cond_broadcast(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static int h2_ios_platform_queue_close(void *user, h2_pal_queue_t *queue) {
  (void)user;
  if (queue == NULL) {
    return H2_PAL_QUEUE_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&queue->mutex);
  queue->closed = 1;
  pthread_cond_broadcast(&queue->not_empty);
  pthread_cond_broadcast(&queue->not_full);
  pthread_mutex_unlock(&queue->mutex);
  return H2_PAL_QUEUE_OK;
}

static const h2_pal_queue_vtable_t s_h2_ios_platform_queue_vtable = {
    .create = h2_ios_platform_queue_create,
    .destroy = h2_ios_platform_queue_destroy,
    .send = h2_ios_platform_queue_send,
    .send_latest = h2_ios_platform_queue_send_latest,
    .recv = h2_ios_platform_queue_recv,
    .reset = h2_ios_platform_queue_reset,
    .close = h2_ios_platform_queue_close,
};
static const h2_pal_queue_api_t s_h2_ios_platform_queue = {
    .user = NULL,
    .vtable = &s_h2_ios_platform_queue_vtable,
};

static int h2_ios_platform_display_open(void *user) {
  h2_ios_platform_t *host = user;
  if (host == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  if (host->rgba == NULL) {
    host->rgba = calloc((size_t)host->width * host->height, sizeof(uint32_t));
  }
  host->opened = host->rgba != NULL;
  pthread_mutex_unlock(&host->mutex);
  return host->opened ? H2_DISPLAY_OK : H2_DISPLAY_ERR_NO_MEMORY;
}

static int h2_ios_platform_display_get_info(void *user,
                                            h2_display_info_t *out_info) {
  h2_ios_platform_t *host = user;
  if (host == NULL || out_info == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  *out_info = (h2_display_info_t){
      .width = host->width,
      .height = host->height,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  return H2_DISPLAY_OK;
}

static int
h2_ios_platform_display_draw_bitmap(void *user, const h2_display_rect_t *rect,
                                    const void *pixels, size_t stride_bytes,
                                    h2_display_pixel_format_t format) {
  h2_ios_platform_t *host = user;
  if (host == NULL || rect == NULL || pixels == NULL || host->rgba == NULL ||
      format != H2_DISPLAY_PIXEL_RGB565 || rect->x < 0 || rect->y < 0 ||
      rect->width <= 0 || rect->height <= 0 ||
      rect->x + rect->width > host->width ||
      rect->y + rect->height > host->height ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t)) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  for (int y = 0; y < rect->height; ++y) {
    const uint16_t *source =
        (const uint16_t *)((const uint8_t *)pixels + (size_t)y * stride_bytes);
    uint32_t *destination =
        host->rgba + (size_t)(rect->y + y) * host->width + rect->x;
    for (int x = 0; x < rect->width; ++x) {
      const uint16_t pixel = source[x];
      const uint32_t red = ((pixel >> 11u) & 0x1fu) * 255u / 31u;
      const uint32_t green = ((pixel >> 5u) & 0x3fu) * 255u / 63u;
      const uint32_t blue = (pixel & 0x1fu) * 255u / 31u;
      destination[x] = 0xff000000u | (red << 16u) | (green << 8u) | blue;
    }
  }
  pthread_mutex_unlock(&host->mutex);
  return H2_DISPLAY_OK;
}

static int h2_ios_platform_display_present(void *user) {
  h2_ios_platform_t *host = user;
  if (host == NULL || host->view == nil) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  UIView *view = host->view;
  dispatch_async(dispatch_get_main_queue(), ^{
    [view setNeedsDisplay];
  });
  return H2_DISPLAY_OK;
}

static int h2_ios_platform_display_set_brightness(void *user,
                                                  uint32_t percent) {
  return user != NULL && percent <= 100u ? H2_DISPLAY_OK
                                         : H2_DISPLAY_ERR_INVALID_ARG;
}

static int h2_ios_platform_display_close(void *user) {
  h2_ios_platform_t *host = user;
  if (host == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  free(host->rgba);
  host->rgba = NULL;
  host->opened = 0;
  pthread_mutex_unlock(&host->mutex);
  return H2_DISPLAY_OK;
}

static const h2_pal_display_vtable_t s_h2_ios_platform_display_vtable = {
    .open = h2_ios_platform_display_open,
    .get_info = h2_ios_platform_display_get_info,
    .draw_bitmap = h2_ios_platform_display_draw_bitmap,
    .present = h2_ios_platform_display_present,
    .set_brightness_percent = h2_ios_platform_display_set_brightness,
    .close = h2_ios_platform_display_close,
};

h2_ios_platform_t *
h2_ios_platform_create(const h2_ios_platform_config_t *config) {
  if (config == NULL || config->display_width <= 0 ||
      config->display_height <= 0) {
    return NULL;
  }
  h2_ios_platform_t *host = calloc(1u, sizeof(*host));
  if (host == NULL || pthread_mutex_init(&host->mutex, NULL) != 0) {
    free(host);
    return NULL;
  }
  host->width = config->display_width;
  host->height = config->display_height;
  host->display.user = host;
  host->display.vtable = &s_h2_ios_platform_display_vtable;
  host->view = [[H2IOSPlatformView alloc] initWithHost:host];
  if (host->view == nil) {
    (void)pthread_mutex_destroy(&host->mutex);
    free(host);
    return NULL;
  }
  return host;
}

void h2_ios_platform_destroy(h2_ios_platform_t *host) {
  if (host == NULL) {
    return;
  }
  (void)h2_ios_platform_display_close(host);
  H2IOSPlatformView *view = host->view;
  host->view = nil;
  void (^detach_view)(void) = ^{
    [view detachHost];
    [view removeFromSuperview];
  };
  if (NSThread.isMainThread) {
    detach_view();
  } else {
    dispatch_sync(dispatch_get_main_queue(), detach_view);
  }
  (void)pthread_mutex_destroy(&host->mutex);
  free(host);
}

UIView *h2_ios_platform_view(h2_ios_platform_t *host) {
  return host == NULL ? nil : host->view;
}

const h2_pal_mem_api_t *h2_ios_platform_mem_api(void) {
  return &s_h2_ios_platform_mem;
}

const h2_pal_time_api_t *h2_ios_platform_time_api(void) {
  return &s_h2_ios_platform_time;
}

const h2_pal_queue_api_t *h2_ios_platform_queue_api(void) {
  return &s_h2_ios_platform_queue;
}

const h2_pal_display_api_t *
h2_ios_platform_display_api(h2_ios_platform_t *host) {
  return host == NULL ? NULL : &host->display;
}

h2_pal_result_t h2_ios_platform_read_pointer(void *user, int32_t *out_x,
                                             int32_t *out_y, int *out_pressed) {
  h2_ios_platform_t *host = user;
  if (host == NULL || out_x == NULL || out_y == NULL || out_pressed == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  *out_x = host->pointer_x;
  *out_y = host->pointer_y;
  *out_pressed = host->pointer_pressed;
  pthread_mutex_unlock(&host->mutex);
  return H2_PAL_OK;
}
