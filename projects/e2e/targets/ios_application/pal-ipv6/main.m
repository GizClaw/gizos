#include "h2_ios_net.h"
#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>
#include <stdlib.h>
#include <string.h>

typedef struct mobile_owner {
  h2_ios_net_t *net;
  h2_ios_webrtc_t *webrtc;
  h2_pal_ipv6_result_t result;
  char *report_path;
  int rc;
} mobile_owner_t;
static mobile_owner_t *retained_owner;
static int cleanup_owner(mobile_owner_t *owner) {
  int rc = h2_ipv6_mobile_cleanup(&owner->result);
  if (rc == H2_PAL_OK)
    rc = h2_ios_webrtc_destroy(&owner->webrtc);
  if (rc == H2_PAL_OK)
    rc = h2_ios_net_destroy(&owner->net);
  if (rc == H2_PAL_OK)
    rc = h2_ios_platform_core_shutdown();
  return rc;
}
static void release_owner(mobile_owner_t *owner) {
  free(owner->report_path);
  free(owner);
}
static dispatch_queue_t ipv6_run_queue(void) {
  static dispatch_queue_t queue;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ queue = dispatch_queue_create("pal-ipv6.run", DISPATCH_QUEUE_SERIAL); });
  return queue;
}

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
  dispatch_async(ipv6_run_queue(), ^{
    @autoreleasepool {
      NSData *data = [NSData
          dataWithContentsOfFile:
              [documents stringByAppendingPathComponent:@"fixture.json"]];
      NSDictionary *fixture = data
                                  ? [NSJSONSerialization JSONObjectWithData:data
                                                                    options:0
                                                                      error:nil]
                                  : nil;
      if (retained_owner) {
        int cleanup = cleanup_owner(retained_owner);
        (void)h2_ipv6_write_report(retained_owner->report_path, "ios-simulator",
            &retained_owner->result, retained_owner->rc, cleanup);
        if (cleanup != H2_PAL_OK) {
          dispatch_async(dispatch_get_main_queue(), ^{
            view.text = [NSString stringWithFormat:@"PAL IPv6: FAIL (%d)", cleanup];
          });
          return;
        }
        release_owner(retained_owner);
        retained_owner = NULL;
      }
      mobile_owner_t *owner = calloc(1u, sizeof(*owner));
      if (!owner)
        return;
      owner->report_path = malloc(strlen(report.fileSystemRepresentation) + 1u);
      if (!owner->report_path) {
        free(owner);
        return;
      }
      strcpy(owner->report_path, report.fileSystemRepresentation);
      NSData *pem = [fixture[@"ca"] dataUsingEncoding:NSUTF8StringEncoding];
      NSData *wrong =
          [fixture[@"wrong_ca"] dataUsingEncoding:NSUTF8StringEncoding];
      int rc =
          pem && wrong ? h2_ios_net_create(&owner->net) : H2_PAL_ERR_INVALID_ARG;
      if (rc == H2_PAL_OK)
        rc = h2_ios_webrtc_create(&owner->webrtc);
      if (rc == H2_PAL_OK) {
        h2_runtime_config_t config = h2_ios_app_host_config();
        config.net = h2_ios_net_api(owner->net);
        config.webrtc = h2_ios_webrtc_api(owner->webrtc);
        rc = h2_ipv6_mobile_run(
            config, [fixture[@"host"] UTF8String],
            (uint16_t)[fixture[@"port"] unsignedIntValue],
            [fixture[@"session"] UTF8String], pem.bytes, pem.length,
            wrong.bytes, wrong.length, [fixture[@"dns_host"] UTF8String],
            [fixture[@"dns_ip"] UTF8String], [fixture[@"http_url"] UTF8String],
            [fixture[@"fallback_url"] UTF8String],
            (uint16_t)[fixture[@"mqtt_port"] unsignedIntValue],
            [fixture[@"offer"] UTF8String], [fixture[@"stun"] UTF8String],
            (uint16_t)[fixture[@"dns_port"] unsignedIntValue], &owner->result);
      }
      owner->rc = rc;
      int teardown = cleanup_owner(owner);
      int written = h2_ipv6_write_report(owner->report_path, "ios-simulator",
                                         &owner->result, rc, teardown);
      if (rc == H2_PAL_OK && teardown != H2_PAL_OK)
        rc = teardown;
      if (written != H2_PAL_OK)
        rc = written;
      if (teardown != H2_PAL_OK)
        retained_owner = owner;
      else
        release_owner(owner);
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
