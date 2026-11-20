#import "h2_ios_platform.h"

#import <XCTest/XCTest.h>

@interface H2TestTouch : UITouch
@property(nonatomic) CGPoint point;
@end
@implementation H2TestTouch
- (CGPoint)locationInView:(UIView *)view {
  (void)view;
  return self.point;
}
@end

@interface H2IOSPlatformTests : XCTestCase
@end

extern void h2_ios_test_corebluetooth(void);
extern void h2_ios_test_corebluetooth_uuid(void);
extern void h2_ios_test_system_event(void);

@implementation H2IOSPlatformTests

- (void)testIndependentCoreBluetoothProvider {
  h2_ios_test_corebluetooth();
  h2_ios_test_corebluetooth_uuid();
}

- (void)testIndependentSystemEventProvider {
  h2_ios_test_system_event();
}

- (void)testInvalidConfigDoesNotCreatePlatform {
  XCTAssertEqual(h2_ios_platform_create(NULL), NULL);
  const h2_ios_platform_config_t invalid_config = {
      .display_width = 0,
      .display_height = 2,
  };
  XCTAssertEqual(h2_ios_platform_create(&invalid_config), NULL);
}

- (void)testQueueWithoutAllocatorIsRejected {
  const h2_pal_queue_config_t config = {
      .allocator = NULL,
      .item_size = sizeof(uint32_t),
      .item_count = 1u,
  };
  h2_pal_queue_t *queue = NULL;
  XCTAssertEqual(h2_pal_queue_create(h2_ios_platform_queue_api(), &config, &queue),
                 H2_PAL_QUEUE_ERR_INVALID_ARG);
  XCTAssertEqual(queue, NULL);
}

- (void)testQueuedTouchGestureInAspectFitViewport {
  const h2_ios_platform_config_t config = {.display_width = 480, .display_height = 800};
  h2_ios_platform_t *platform = h2_ios_platform_create(&config);
  XCTAssertNotEqual(platform, NULL);
  UIView *view = h2_ios_platform_view(platform);
  view.frame = CGRectMake(0, 0, 800, 800);
  const h2_pal_touch_api_t *touch = h2_ios_platform_touch_api(platform);
  h2_pal_touch_sample_t event;
  XCTAssertEqual(h2_pal_touch_poll_pointer(touch, &event), H2_PAL_ERR_INVALID_STATE);
  XCTAssertEqual(h2_pal_touch_open(touch), H2_PAL_OK);
  XCTAssertEqual(h2_pal_touch_open(touch), H2_PAL_ERR_BUSY);
  h2_pal_touch_info_t info;
  XCTAssertEqual(h2_pal_touch_get_info(touch, &info), H2_PAL_OK);
  XCTAssertEqual(info.width, 480u);
  XCTAssertEqual(info.height, 800u);
  H2TestTouch *finger = [H2TestTouch new];
  NSSet<UITouch *> *points = [NSSet setWithObject:finger];
  finger.point = CGPointMake(200, 100);
  [view touchesBegan:points withEvent:nil];
  finger.point = CGPointMake(400, 200);
  [view touchesMoved:points withEvent:nil];
  [view touchesCancelled:points withEvent:nil];
  [view touchesBegan:points withEvent:nil];
  [view touchesEnded:points withEvent:nil];
  const h2_pal_touch_event_kind_t kinds[] = {H2_PAL_TOUCH_EVENT_DOWN, H2_PAL_TOUCH_EVENT_MOVE,
                                             H2_PAL_TOUCH_EVENT_CANCEL, H2_PAL_TOUCH_EVENT_DOWN,
                                             H2_PAL_TOUCH_EVENT_UP};
  for (unsigned i = 0; i < sizeof(kinds) / sizeof(kinds[0]); ++i) {
    XCTAssertEqual(h2_pal_touch_poll_pointer(touch, &event), H2_PAL_OK);
    XCTAssertEqual(event.event.kind, kinds[i]);
    XCTAssertEqual(event.event.x, i == 0 ? 40 : 240);
    XCTAssertEqual(event.event.y, i == 0 ? 100 : 200);
  }
  XCTAssertEqual(h2_pal_touch_poll_pointer(touch, &event), H2_PAL_ERR_WOULD_BLOCK);
  for (int i = 0; i < 100; ++i) {
    [view touchesBegan:points withEvent:nil];
    [view touchesEnded:points withEvent:nil];
  }
  XCTAssertEqual(h2_pal_touch_poll_pointer(touch, &event), H2_PAL_ERR_FULL);
  XCTAssertEqual(h2_pal_touch_close(touch), H2_PAL_OK);
  h2_ios_platform_destroy(platform);
}

- (void)testPresentThenDestroyDetachesQueuedRedraw {
  XCTAssertTrue(NSThread.isMainThread);
  const h2_ios_platform_config_t config = {
      .display_width = 2,
      .display_height = 2,
  };
  h2_ios_platform_t *platform = h2_ios_platform_create(&config);
  XCTAssertNotEqual(platform, NULL);

  UIView *surface = h2_ios_platform_view(platform);
  XCTAssertNotNil(surface);
  UIView *container = [[UIView alloc] initWithFrame:CGRectMake(0, 0, 4, 4)];
  surface.frame = container.bounds;
  [container addSubview:surface];

  const h2_pal_display_api_t *display = h2_ios_platform_display_api(platform);
  XCTAssertEqual(h2_pal_display_open(display), H2_DISPLAY_OK);
  XCTAssertEqual(h2_pal_display_present(display), H2_DISPLAY_OK);

  h2_ios_platform_destroy(platform);
  XCTAssertNil(surface.superview);

  [surface setNeedsDisplay];
  [surface drawRect:surface.bounds];
}

@end
