#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "atomic_mobile_runner.h"
#import <UIKit/UIKit.h>

@interface AtomicViewController : UIViewController
@end
@implementation AtomicViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
  view.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  view.editable = NO;
  view.text = @"Atomic E2E running…";
  [self.view addSubview:view];
  NSString *documents = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSString *report =
      [documents stringByAppendingPathComponent:@"atomic-result.json"];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      NSString *version =
          NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
      int rc = h2_atomic_mobile_run(
          h2_ios_app_host_config(), "ios-simulator", version.UTF8String,
          report.fileSystemRepresentation, h2_ios_platform_core_shutdown);
      NSString *text = [NSString stringWithContentsOfFile:report
                                                 encoding:NSUTF8StringEncoding
                                                    error:nil];
      dispatch_async(dispatch_get_main_queue(), ^{
        view.text = [NSString stringWithFormat:@"Atomic: %@ (%d)\n%@",
                                               rc == 0 ? @"PASS" : @"FAIL", rc,
                                               text ?: @""];
      });
    }
  });
}
@end
@interface AtomicSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation AtomicSceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [AtomicViewController new];
  [self.window makeKeyAndVisible];
}
@end
@interface AtomicAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AtomicAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config =
      [[UISceneConfiguration alloc] initWithName:@"Default"
                                     sessionRole:session.role];
  config.delegateClass = AtomicSceneDelegate.class;
  return config;
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(AtomicAppDelegate.class));
  }
}
