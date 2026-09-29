#include "h2_ios_net.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>

@interface NetTLSViewController : UIViewController
@end
@implementation NetTLSViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
  view.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  view.editable = NO;
  view.text = @"PAL Net/TLS E2E running…";
  [self.view addSubview:view];
  NSString *documents = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSString *report =
      [documents stringByAppendingPathComponent:@"pal-net-tls-result.json"];
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
      h2_net_tls_result_t result = {0};
      h2_ios_net_t *owner = NULL;
      NSData *pem = [fixture[@"ca"] dataUsingEncoding:NSUTF8StringEncoding];
      NSData *wrong =
          [fixture[@"wrong_ca"] dataUsingEncoding:NSUTF8StringEncoding];
      int rc =
          pem && wrong ? h2_ios_net_create(&owner) : H2_PAL_ERR_INVALID_ARG;
      if (rc == H2_PAL_OK) {
        h2_runtime_config_t config = h2_ios_app_host_config();
        config.net = h2_ios_net_api(owner);
        rc = h2_net_tls_mobile_run(
            config, [fixture[@"host"] UTF8String],
            (uint16_t)[fixture[@"port"] unsignedIntValue],
            [fixture[@"session"] UTF8String], pem.bytes, pem.length,
            wrong.bytes, wrong.length, &result);
      }
      int teardown = h2_ios_net_destroy(&owner);
      if (teardown == H2_PAL_OK)
        teardown = h2_ios_platform_core_shutdown();
      int written =
          h2_net_tls_write_report(report.fileSystemRepresentation,
                                  "ios-simulator", &result, rc, teardown);
      if (written != H2_PAL_OK)
        rc = written;
      NSString *text = [NSString stringWithContentsOfFile:report
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
      dispatch_async(dispatch_get_main_queue(), ^{
        view.text = [NSString stringWithFormat:@"PAL Net/TLS: %@ (%d)\n%@",
                                               rc == 0 ? @"PASS" : @"FAIL", rc,
                                               text ?: @""];
      });
    }
  });
}
@end
@interface NetTLSSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation NetTLSSceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [NetTLSViewController new];
  [self.window makeKeyAndVisible];
}
@end
@interface NetTLSAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation NetTLSAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config =
      [[UISceneConfiguration alloc] initWithName:@"Default"
                                     sessionRole:session.role];
  config.delegateClass = NetTLSSceneDelegate.class;
  return config;
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(NetTLSAppDelegate.class));
  }
}
