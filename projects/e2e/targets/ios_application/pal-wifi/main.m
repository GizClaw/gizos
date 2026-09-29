#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>

@interface WiFiViewController : UIViewController
@end
@implementation WiFiViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
  view.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  view.editable = NO;
  view.text = @"PAL WiFi E2E running…";
  [self.view addSubview:view];
  NSString *documents = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSString *report =
      [documents stringByAppendingPathComponent:@"pal-wifi-result.json"];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      NSString *version =
          NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
      int rc = h2_wifi_mobile_run(
          h2_ios_app_host_config(), "ios-simulator", version.UTF8String,
          report.fileSystemRepresentation, h2_ios_platform_core_shutdown);
      NSString *text = [NSString stringWithContentsOfFile:report
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
      dispatch_async(dispatch_get_main_queue(), ^{
        view.text = [NSString stringWithFormat:@"PAL WiFi: %@ (%d)\n%@",
                                               rc == 0 ? @"PASS" : @"FAIL", rc,
                                               text ?: @""];
      });
    }
  });
}
@end
@interface WiFiSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation WiFiSceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [WiFiViewController new];
  [self.window makeKeyAndVisible];
}
@end
@interface WiFiAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation WiFiAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config =
      [[UISceneConfiguration alloc] initWithName:@"Default"
                                     sessionRole:session.role];
  config.delegateClass = WiFiSceneDelegate.class;
  return config;
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(WiFiAppDelegate.class));
  }
}
