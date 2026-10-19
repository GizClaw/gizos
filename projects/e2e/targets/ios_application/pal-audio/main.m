#include "h2_ios_platform.h"
#include "mobile_runner.h"

#import <AVFoundation/AVFoundation.h>
#import <UIKit/UIKit.h>

@interface AudioViewController : UIViewController
@end
@implementation AudioViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
  view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  view.editable = NO;
  view.text = @"PAL Audio E2E running…";
  [self.view addSubview:view];
  NSString *documents = NSSearchPathForDirectoriesInDomains(
      NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
  NSString *path = [documents stringByAppendingPathComponent:@"pal-audio-result.json"];
  void (^permissionResult)(BOOL) = ^(BOOL granted) {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
      @autoreleasepool {
        NSString *version = NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
        h2_pal_audio_e2e_result_t result = {0};
        h2_ios_audio_t *owner = NULL;
        int rc = granted ? h2_ios_audio_create(&owner) : H2_AUDIO_ERR_UNAVAILABLE;
        if (rc == H2_AUDIO_OK)
          rc = h2_audio_mobile_run(h2_ios_audio_api(owner),
                                    h2_ios_platform_time_api(), &result);
        h2_ios_audio_destroy(owner);
        const int teardown = h2_ios_platform_core_shutdown();
        const int verdict = h2_audio_mobile_report(path.fileSystemRepresentation,
            "ios-simulator", version.UTF8String, &result, rc, teardown);
        NSString *text = [NSString stringWithContentsOfFile:path
            encoding:NSUTF8StringEncoding error:nil];
        dispatch_async(dispatch_get_main_queue(), ^{
          view.text = [NSString stringWithFormat:@"PAL Audio: %@ (%d)\n%@",
              verdict == 0 ? @"PASS" : @"FAIL", verdict, text ?: @""];
        });
      }
    });
  };
  if (@available(iOS 17.0, *)) {
    [AVAudioApplication requestRecordPermissionWithCompletionHandler:permissionResult];
  } else {
    /* The legacy API is required only for the supported iOS 16 runtime. */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [[AVAudioSession sharedInstance] requestRecordPermission:permissionResult];
#pragma clang diagnostic pop
  }
}
@end

@interface AudioSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation AudioSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session
        options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [AudioViewController new];
  [self.window makeKeyAndVisible];
}
@end

@interface AudioAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation AudioAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                 options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config = [[UISceneConfiguration alloc]
      initWithName:@"Default" sessionRole:session.role];
  config.delegateClass = AudioSceneDelegate.class;
  return config;
}
@end

int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(AudioAppDelegate.class));
  }
}
