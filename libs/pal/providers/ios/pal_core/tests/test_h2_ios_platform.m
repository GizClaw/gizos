#import "h2_ios_platform.h"

#import <XCTest/XCTest.h>

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
  XCTAssertEqual(
      h2_pal_queue_create(h2_ios_platform_queue_api(), &config, &queue),
      H2_PAL_QUEUE_ERR_INVALID_ARG);
  XCTAssertEqual(queue, NULL);
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
