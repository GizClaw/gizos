#import "h2_ios_platform.h"

#import <CoreGraphics/CoreGraphics.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  uint32_t brightness_percent;
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
  const CGFloat brightness = host->brightness_percent / 100.0;
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
    CGContextSetRGBFillColor(context, 0, 0, 0, 1);
    CGContextFillRect(context, self.bounds);
    CGContextSetAlpha(context, brightness);
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

static int h2_ios_platform_display_open(void *user) {
  h2_ios_platform_t *host = user;
  if (host == NULL) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  if (host->rgba == NULL) {
    host->rgba = calloc((size_t)host->width * host->height, sizeof(uint32_t));
  }
  if (!host->opened) host->brightness_percent = 100u;
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
  pthread_mutex_lock(&host->mutex);
  if (!host->opened) {
    pthread_mutex_unlock(&host->mutex);
    return H2_DISPLAY_ERR_INVALID_STATE;
  }
  *out_info = (h2_display_info_t){
      .width = host->width,
      .height = host->height,
      .native_format = H2_DISPLAY_PIXEL_RGB565,
  };
  pthread_mutex_unlock(&host->mutex);
  return H2_DISPLAY_OK;
}

static int
h2_ios_platform_display_draw_bitmap(void *user, const h2_display_rect_t *rect,
                                    const void *pixels, size_t stride_bytes,
                                    h2_display_pixel_format_t format) {
  h2_ios_platform_t *host = user;
  if (host && !host->opened) return H2_DISPLAY_ERR_INVALID_STATE;
  if (format != H2_DISPLAY_PIXEL_RGB565) return H2_DISPLAY_ERR_UNSUPPORTED;
  if (host == NULL || rect == NULL || pixels == NULL || rect->x < 0 || rect->y < 0 ||
      rect->width <= 0 || rect->height <= 0 ||
      (int64_t)rect->x + rect->width > host->width ||
      (int64_t)rect->y + rect->height > host->height ||
      stride_bytes < (size_t)rect->width * sizeof(uint16_t) ||
      ((size_t)rect->height - 1u) > (SIZE_MAX - (size_t)rect->width*2u) / stride_bytes) {
    return H2_DISPLAY_ERR_INVALID_ARG;
  }
  pthread_mutex_lock(&host->mutex);
  for (int y = 0; y < rect->height; ++y) {
    const uint8_t *source =
        (const uint8_t *)pixels + (size_t)y * stride_bytes;
    uint32_t *destination =
        host->rgba + (size_t)(rect->y + y) * host->width + rect->x;
    for (int x = 0; x < rect->width; ++x) {
      uint16_t pixel;
      memcpy(&pixel, source + (size_t)x*2u, 2u);
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
  if (!host->opened) return H2_DISPLAY_ERR_INVALID_STATE;
  UIView *view = host->view;
  dispatch_async(dispatch_get_main_queue(), ^{
    [view setNeedsDisplay];
  });
  return H2_DISPLAY_OK;
}

static int h2_ios_platform_display_set_brightness(void *user,
                                                  uint32_t percent) {
  h2_ios_platform_t *host = user;
  if (!host) return H2_DISPLAY_ERR_INVALID_ARG;
  pthread_mutex_lock(&host->mutex);
  int rc = !host->opened ? H2_DISPLAY_ERR_INVALID_STATE :
      percent > 100u ? H2_DISPLAY_ERR_INVALID_ARG : H2_DISPLAY_OK;
  if (!rc) host->brightness_percent = percent;
  pthread_mutex_unlock(&host->mutex);
  return rc ? rc : h2_ios_platform_display_present(host);
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
