#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>
#include <stdio.h>

@interface GizClawViewController : UIViewController
@end
@implementation GizClawViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    view.editable = NO;
    view.text = @"GizClaw E2E running…";
    [self.view addSubview:view];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *report = [documents stringByAppendingPathComponent:@"gizclaw-result.json"];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSData *data = [NSData dataWithContentsOfFile:[documents stringByAppendingPathComponent:@"fixture.json"]];
            NSDictionary *fixture = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            h2_gizclaw_e2e_result_t result = {0};
            NSData *pcm = [NSData dataWithContentsOfFile:[documents stringByAppendingPathComponent:@"voice.pcm"]];
            FILE *log = freopen([[documents stringByAppendingPathComponent:@"gizclaw.log"] fileSystemRepresentation], "w", stdout);
            if (log) setvbuf(stdout, NULL, _IOLBF, 0);
            h2_ios_http_t *http = NULL;
            h2_ios_webrtc_t *owner = NULL;
            int rc = fixture && pcm && log && h2_gizclaw_e2e_fixture_key()[0] &&
                h2_gizclaw_e2e_fixture_profile()[0] && h2_gizclaw_e2e_fixture_value()[0] ? h2_ios_http_create(NULL, 0u, &http) : H2_PAL_ERR_INVALID_ARG;
            if (rc == H2_PAL_OK) rc = h2_ios_webrtc_create(&owner);
            if (rc == H2_PAL_OK) {
                h2_runtime_config_t config = h2_ios_app_host_config();
                config.http = h2_ios_http_api(http);
                config.webrtc = h2_ios_webrtc_api(owner);
                rc = h2_gizclaw_mobile_run(config, "ios-simulator", [fixture[@"endpoint"] UTF8String],
                    [fixture[@"token"] UTF8String], [fixture[@"api_url"] UTF8String],
                    [fixture[@"audio_url"] UTF8String], pcm.bytes, pcm.length, &result);
            }
            int teardown = H2_PAL_ERR_INVALID_STATE;
            if (!result.retained_resources) {
                teardown = h2_ios_webrtc_destroy(&owner);
                h2_ios_http_destroy(http);
                if (teardown == H2_PAL_OK) teardown = h2_ios_platform_core_shutdown();
            }
            rc = h2_gizclaw_mobile_report(report.fileSystemRepresentation, "ios-simulator", &result, rc, teardown);
            fflush(stdout);
            NSString *text = [NSString stringWithContentsOfFile:report encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                view.text = [NSString stringWithFormat:@"GizClaw: %@ (%d)\n%@", rc == 0 ? @"PASS" : @"FAIL", rc, text ?: @""];
            });
        }
    });
    });
}
@end
@interface GizClawSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation GizClawSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [GizClawViewController new];
    [self.window makeKeyAndVisible];
}
@end
@interface GizClawAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation GizClawAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)app; (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = GizClawSceneDelegate.class;
    return config;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(GizClawAppDelegate.class)); }
}
