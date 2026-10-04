#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>

@interface MQTTViewController : UIViewController
@end
@implementation MQTTViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    UITextView *view = [[UITextView alloc] initWithFrame:self.view.bounds];
    view.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    view.editable = NO;
    view.text = @"PAL MQTT E2E running…";
    [self.view addSubview:view];
    NSString *documents = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES).firstObject;
    NSString *report = [documents stringByAppendingPathComponent:@"pal-mqtt-result.json"];
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        @autoreleasepool {
            NSString *version = NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
            NSData *data = [NSData dataWithContentsOfFile:[documents stringByAppendingPathComponent:@"fixture.json"]];
            NSDictionary *fixture = data ? [NSJSONSerialization JSONObjectWithData:data options:0 error:nil] : nil;
            h2_mqtt_mobile_result_t result = {0};
            NSString *ca = fixture[@"ca"], *wrong = fixture[@"wrong_ca"];
            NSData *pem = [ca dataUsingEncoding:NSUTF8StringEncoding];
            NSData *wrong_pem = [wrong dataUsingEncoding:NSUTF8StringEncoding];
            h2_runtime_config_t config = h2_ios_app_host_config();
            int rc = fixture ? h2_mqtt_mobile_run(config, [fixture[@"host"] UTF8String],
                [fixture[@"tcp_port"] unsignedShortValue], [fixture[@"tls_port"] unsignedShortValue],
                [fixture[@"session"] UTF8String], pem.bytes, pem.length, wrong_pem.bytes, wrong_pem.length, &result)
                : H2_PAL_ERR_INVALID_ARG;
            int teardown = h2_ios_platform_core_shutdown();
            rc = h2_mqtt_mobile_report(report.fileSystemRepresentation, "ios-simulator",
                version.UTF8String, &result, rc, teardown);
            NSString *text = [NSString stringWithContentsOfFile:report encoding:NSUTF8StringEncoding error:nil];
            dispatch_async(dispatch_get_main_queue(), ^{
                view.text = [NSString stringWithFormat:@"PAL MQTT: %@ (%d)\n%@", rc == 0 ? @"PASS" : @"FAIL", rc, text ?: @""];
            });
        }
    });
}
@end
@interface MQTTSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation MQTTSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)session; (void)options;
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [MQTTViewController new];
    [self.window makeKeyAndVisible];
}
@end
@interface MQTTAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation MQTTAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    (void)app; (void)options;
    UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    config.delegateClass = MQTTSceneDelegate.class;
    return config;
}
@end
int main(int argc, char **argv) {
    @autoreleasepool { return UIApplicationMain(argc, argv, nil, NSStringFromClass(MQTTAppDelegate.class)); }
}
