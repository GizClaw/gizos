#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>

@interface HTTPViewController : UIViewController
@end
@implementation HTTPViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    view.editable = NO;
    view.text = @"PAL HTTP E2E running…";
    [self.view addSubview:view];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *report = [documents stringByAppendingPathComponent:@"pal-http-result.json"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSString *version = NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
            NSData *data = [NSData dataWithContentsOfFile:[documents stringByAppendingPathComponent:@"fixture.json"]];
            NSDictionary *fixture = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            h2_pal_http_e2e_result_t result = {0};
            h2_ios_http_t *owner = NULL;
            NSString *ca = fixture[@"ca"];
            NSData *pem = [ca dataUsingEncoding:NSUTF8StringEncoding];
            int rc = pem ? h2_ios_http_create(pem.bytes, pem.length, &owner) : H2_PAL_ERR_INVALID_ARG;
            if (rc == H2_PAL_OK) {
                h2_runtime_config_t config = h2_ios_app_host_config();
                config.http = h2_ios_http_api(owner);
                rc = h2_http_mobile_run(config, [fixture[@"http"] UTF8String],
                    [fixture[@"https"] UTF8String], [fixture[@"untrusted"] UTF8String], &result);
            }
            h2_ios_http_destroy(owner);
            int teardown = h2_ios_platform_core_shutdown();
            rc = h2_http_mobile_report(report.fileSystemRepresentation, "ios-simulator",
                version.UTF8String, &result, rc, teardown);
            NSString *text = [NSString stringWithContentsOfFile:report encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                view.text = [NSString stringWithFormat:@"PAL HTTP: %@ (%d)\n%@", rc == 0 ? @"PASS" : @"FAIL", rc, text ?: @""];
            });
        }
    });
}
@end
@interface HTTPSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation HTTPSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [HTTPViewController new];
    [self.window makeKeyAndVisible];
}
@end
@interface HTTPAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation HTTPAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)app; (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = HTTPSceneDelegate.class;
    return config;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(HTTPAppDelegate.class)); }
}
