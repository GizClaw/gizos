#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>

@interface WebRTCViewController : UIViewController
@end
@implementation WebRTCViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    view.editable = NO;
    view.text = @"PAL WebRTC E2E running…";
    [self.view addSubview:view];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *report = [documents stringByAppendingPathComponent:@"pal-webrtc-result.json"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSString *version = NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
            NSData *data = [NSData dataWithContentsOfFile:[documents stringByAppendingPathComponent:@"fixture.json"]];
            NSDictionary *fixture = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            h2_pal_webrtc_e2e_result_t result = {0};
            h2_ios_http_t *http = NULL;
            h2_ios_webrtc_t *owner = NULL;
            int rc = fixture ? h2_ios_http_create(NULL, 0u, &http) : H2_PAL_ERR_INVALID_ARG;
            if (rc == H2_PAL_OK) rc = h2_ios_webrtc_create(&owner);
            if (rc == H2_PAL_OK) {
                h2_runtime_config_t config = h2_ios_app_host_config();
                config.http = h2_ios_http_api(http);
                config.webrtc = h2_ios_webrtc_api(owner);
                rc = h2_webrtc_mobile_run(config, [fixture[@"offer"] UTF8String],
                    [fixture[@"stun"] UTF8String], &result);
            }
            int teardown = h2_ios_webrtc_destroy(&owner);
            h2_ios_http_destroy(http);
            if (teardown == H2_PAL_OK) teardown = h2_ios_platform_core_shutdown();
            rc = h2_webrtc_mobile_report(report.fileSystemRepresentation, "ios-simulator",
                version.UTF8String, &result, rc, teardown);
            NSString *text = [NSString stringWithContentsOfFile:report encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                view.text = [NSString stringWithFormat:@"PAL WebRTC: %@ (%d)\n%@", rc == 0 ? @"PASS" : @"FAIL", rc, text ?: @""];
            });
        }
    });
}
@end
@interface WebRTCSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation WebRTCSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [WebRTCViewController new];
    [self.window makeKeyAndVisible];
}
@end
@interface WebRTCAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation WebRTCAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)app; (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = WebRTCSceneDelegate.class;
    return config;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(WebRTCAppDelegate.class)); }
}
