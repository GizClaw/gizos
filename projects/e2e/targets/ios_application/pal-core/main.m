#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "mobile_runner.h"
#import <UIKit/UIKit.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int log_read = -1, saved_stderr = -1;
static h2_pal_result_t log_seen(void *user, h2_pal_log_level_t level,
                                const char *scope, const char *message) {
  (void)user;
  static const char *names[] = {"debug", "info", "warn", "error"};
  char expected[512], text[4096];
  size_t used = 0;
  fflush(stderr);
  for (;;) {
    ssize_t n = read(log_read, text + used, sizeof(text) - used - 1);
    if (n <= 0)
      break;
    used += (size_t)n;
    if (used == sizeof(text) - 1)
      break;
  }
  text[used] = 0;
  snprintf(expected, sizeof(expected), "H2_LOG level=%s scope=%s message=%s\n",
           names[level], scope, message);
  return strstr(text, expected) ? H2_PAL_OK : H2_PAL_ERR_INVALID_STATE;
}
static h2_pal_result_t resources(void *user, h2_pal_core_resources_t *out) {
  (void)user;
  h2_ios_platform_resource_stats_t v;
  int rc = h2_ios_platform_get_resource_stats(&v);
  if (rc)
    return rc;
  *out = (h2_pal_core_resources_t){.tasks = v.tasks,
                                   .task_stack_bytes = v.task_stack_bytes,
                                   .queues = v.queues,
                                   .mutexes = v.mutexes,
                                   .semaphores = v.semaphores,
                                   .conditions = v.conditions,
                                   .timers = v.timers,
                                   .firmware_infos = 1,
                                   .allocations = v.allocations,
                                   .allocation_bytes = v.allocation_bytes};
  return H2_PAL_OK;
}
static h2_pal_result_t fault(void *user, int after) {
  (void)user;
  return h2_ios_platform_task_allocation_fault(after);
}

@interface H2CoreViewController : UIViewController
@end
@implementation H2CoreViewController {
  UITextView *_report;
}
- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.systemBackgroundColor;
  _report = [[UITextView alloc] initWithFrame:self.view.bounds];
  _report.autoresizingMask =
      UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  _report.editable = NO;
  _report.font = [UIFont monospacedSystemFontOfSize:13
                                             weight:UIFontWeightRegular];
  _report.text = @"PAL Core E2E — running 41 cases…";
  [self.view addSubview:_report];
  NSString *directory = NSSearchPathForDirectoriesInDomains(
                            NSDocumentDirectory, NSUserDomainMask, YES)
                            .firstObject;
  NSString *path =
      [directory stringByAppendingPathComponent:@"pal-core-result.json"];
  NSString *version =
      NSBundle.mainBundle.infoDictionary[@"CFBundleShortVersionString"];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      int pipes[2];
      if (pipe(pipes) != 0)
        return;
      log_read = pipes[0];
      fcntl(log_read, F_SETFL, O_NONBLOCK);
      saved_stderr = dup(STDERR_FILENO);
      dup2(pipes[1], STDERR_FILENO);
      close(pipes[1]);
      const h2_mobile_core_fixture_t fixture = {
          .platform = "ios-simulator",
          .image_version = version.UTF8String,
          .runtime_config = h2_ios_app_host_config(),
          .tests = {.event_fixture = h2_ios_system_event_api(),
                    .observe_log = log_seen,
                    .observe_resources = resources,
                    .task_allocation_fault = fault},
          .shutdown = h2_ios_platform_core_shutdown};
      int rc = h2_mobile_core_run(&fixture, path.fileSystemRepresentation);
      fflush(stderr);
      dup2(saved_stderr, STDERR_FILENO);
      close(saved_stderr);
      close(log_read);
      NSString *report = [NSString stringWithContentsOfFile:path
                                                   encoding:NSUTF8StringEncoding
                                                      error:nil];
      dispatch_async(dispatch_get_main_queue(), ^{
        self->_report.text =
            [NSString stringWithFormat:@"PAL Core E2E: %@\n\n%@",
                                       rc == 0 ? @"PASS" : @"FAIL",
                                       report ?: @"No report"];
      });
    }
  });
}
@end

@interface H2CoreSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation H2CoreSceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = [H2CoreViewController new];
  [self.window makeKeyAndVisible];
}
@end
@interface H2CoreAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation H2CoreAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)app;
  (void)options;
  UISceneConfiguration *config =
      [[UISceneConfiguration alloc] initWithName:@"Default"
                                     sessionRole:session.role];
  config.delegateClass = H2CoreSceneDelegate.class;
  return config;
}
@end
int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(H2CoreAppDelegate.class));
  }
}
