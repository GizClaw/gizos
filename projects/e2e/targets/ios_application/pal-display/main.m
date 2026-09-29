#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "h2_pal_display_e2e.h"
#import <UIKit/UIKit.h>
#include <stdio.h>
static h2_ios_platform_t *platform;
static NSString *directory;
static int observe(void *user, const uint16_t *expected, int width, int height,
                   uint32_t brightness, const char *id) {
  (void)user;
  __block int rc = H2_DISPLAY_ERR_IO;
  dispatch_sync(dispatch_get_main_queue(), ^{
    UIView *view = h2_ios_platform_view(platform);
    /* Capture UIKit's actual layer/view rendering, not its provider buffer. */
    [view setNeedsDisplay];
    [view layoutIfNeeded];
    [view.layer displayIfNeeded];
    UIGraphicsImageRendererFormat *format =
        [UIGraphicsImageRendererFormat defaultFormat];
    format.scale = view.contentScaleFactor;
    format.opaque = YES;
    format.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
    UIGraphicsImageRenderer *renderer =
        [[UIGraphicsImageRenderer alloc] initWithSize:view.bounds.size
                                               format:format];
    UIImage *image =
        [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
          (void)context;
          [view drawViewHierarchyInRect:view.bounds afterScreenUpdates:YES];
        }];
    NSData *png = UIImagePNGRepresentation(image);
    NSString *path = [directory
        stringByAppendingPathComponent:[NSString stringWithFormat:@"%s-%u.png",
                                                                  id,
                                                                  brightness]];
    [png writeToFile:path atomically:YES];
    uint8_t *pixels = calloc((size_t)width * height, 4);
    if (pixels) {
      CGColorSpaceRef color = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      CGContextRef bitmap = CGBitmapContextCreate(
          pixels, width, height, 8, (size_t)width * 4, color,
          (CGBitmapInfo)((uint32_t)kCGImageAlphaPremultipliedLast |
                         (uint32_t)kCGBitmapByteOrder32Big));
      if (bitmap) {
        CGContextDrawImage(bitmap, CGRectMake(0, 0, width, height),
                           image.CGImage);
        rc = h2_pal_display_e2e_compare(pixels, (size_t)width * 4, 0, expected,
                                        width, height, brightness, 3);
        CGContextRelease(bitmap);
      }
      CGColorSpaceRelease(color);
      free(pixels);
    }
  });
  printf(
      "H2_DISPLAY_OBSERVATION case=%s source=UIKit-view brightness=%u rc=%d\n",
      id, brightness, rc);
  return rc;
}
@interface DisplayViewController : UIViewController
@end
@implementation DisplayViewController
- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  if (platform)
    return;
  directory = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,
                                                  NSUserDomainMask, YES)
                  .firstObject;
  h2_ios_platform_config_t config = {.display_width = 96, .display_height = 80};
  platform = h2_ios_platform_create(&config);
  if (!platform)
    return;
  UIView *surface = h2_ios_platform_view(platform);
  CGFloat scale = self.view.window.screen.scale;
  surface.frame = CGRectMake(0, 80, 96 / scale, 80 / scale);
  surface.contentScaleFactor = scale;
  [self.view addSubview:surface];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      NSString *report =
          [directory stringByAppendingPathComponent:@"pal-display-result.log"];
      if (!freopen(report.fileSystemRepresentation, "w", stdout))
        return;
      h2_runtime_config_t cfg = h2_ios_app_host_config();
      cfg.display = h2_ios_platform_display_api(platform);
      h2_runtime_t *runtime = NULL;
      int rc = h2_runtime_init(&cfg, &runtime);
      h2_pal_display_e2e_result_t result = {0};
      const h2_pal_display_e2e_config_t test = {.supported_formats = 1,
                                                .observe = observe};
      if (!rc)
        rc = h2_pal_display_e2e_run(runtime, &test, &result);
      if (runtime)
        h2_runtime_deinit(runtime);
      h2_ios_platform_destroy(platform);
      platform = NULL;
      int teardown = h2_ios_platform_core_shutdown();
      h2_pal_display_e2e_print(&result, "ios-simulator", rc, teardown);
      fflush(stdout);
    }
  });
}
@end
@interface DisplaySceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation DisplaySceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [DisplayViewController new];
  [self.window makeKeyAndVisible];
}
@end
@interface DisplayAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation DisplayAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config =
      [[UISceneConfiguration alloc] initWithName:@"Default"
                                     sessionRole:session.role];
  config.delegateClass = DisplaySceneDelegate.class;
  return config;
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(DisplayAppDelegate.class));
  }
}
