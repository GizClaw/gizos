#include "h2_ios_net.h"
#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>

@interface IPv6ViewController : UIViewController
@end
@implementation IPv6ViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
  view.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  view.editable = NO;
  view.text = @"PAL IPv6 E2E running…";
  [self.view addSubview:view];
  NSString *documents = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSString *report =
      [documents stringByAppendingPathComponent:@"pal-ipv6-result.json"];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      NSData *data = [NSData
          dataWithContentsOfFile:
              [documents stringByAppendingPathComponent:@"fixture.json"]];
      NSDictionary *fixture = data
                                  ? [NSJSONSerialization JSONObjectWithData:data
                                                                    options:0
                                                                      error:nil]
                                  : nil;
      h2_pal_ipv6_result_t result = {0};
      h2_ios_net_t *owner = NULL;
      h2_ios_webrtc_t *webrtc = NULL;
      NSData *pem = [fixture[@"ca"] dataUsingEncoding:NSUTF8StringEncoding];
      NSData *wrong =
          [fixture[@"wrong_ca"] dataUsingEncoding:NSUTF8StringEncoding];
      int rc =
          pem && wrong ? h2_ios_net_create(&owner) : H2_PAL_ERR_INVALID_ARG;
      if (rc == H2_PAL_OK)
        rc = h2_ios_webrtc_create(&webrtc);
      if (rc == H2_PAL_OK) {
        h2_runtime_config_t config = h2_ios_app_host_config();
        config.net = h2_ios_net_api(owner);
        config.webrtc = h2_ios_webrtc_api(webrtc);
        rc = h2_ipv6_mobile_run(
            config, [fixture[@"host"] UTF8String],
            (uint16_t)[fixture[@"port"] unsignedIntValue],
            [fixture[@"session"] UTF8String], pem.bytes, pem.length,
            wrong.bytes, wrong.length, [fixture[@"dns_host"] UTF8String],
            [fixture[@"dns_ip"] UTF8String], [fixture[@"http_url"] UTF8String],
            [fixture[@"fallback_url"] UTF8String],
            (uint16_t)[fixture[@"mqtt_port"] unsignedIntValue],
            [fixture[@"offer"] UTF8String], [fixture[@"stun"] UTF8String],
            (uint16_t)[fixture[@"dns_port"] unsignedIntValue], &result);
      }
      int teardown = h2_ios_webrtc_destroy(&webrtc);
      int net_cleanup = h2_ios_net_destroy(&owner);
      if (teardown == H2_PAL_OK)
        teardown = net_cleanup;
      if (teardown == H2_PAL_OK)
        teardown = h2_ios_platform_core_shutdown();
      int written =
          h2_ipv6_write_report(report.fileSystemRepresentation, "ios-simulator",
                               &result, rc, teardown);
      if (written != H2_PAL_OK)
        rc = written;
      NSString *text = [NSString stringWithContentsOfFile:report
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
      dispatch_async(dispatch_get_main_queue(), ^{
        view.text = [NSString stringWithFormat:@"PAL IPv6: %@ (%d)\n%@",
                                               rc == 0 ? @"PASS" : @"FAIL", rc,
                                               text ?: @""];
      });
    }
  });
}
@end
@interface IPv6SceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation IPv6SceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [IPv6ViewController new];
  [self.window makeKeyAndVisible];
}
@end
@interface IPv6AppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation IPv6AppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config =
      [[UISceneConfiguration alloc] initWithName:@"Default"
                                     sessionRole:session.role];
  config.delegateClass = IPv6SceneDelegate.class;
  return config;
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(IPv6AppDelegate.class));
  }
}
