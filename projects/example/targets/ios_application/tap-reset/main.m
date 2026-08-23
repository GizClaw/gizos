#import "h2_ios_platform.h"

#include "h2_mobile_app.h"
#include "h2_smoke_host_runtime.h"

#import <UIKit/UIKit.h>

#include <pthread.h>
#include <stdatomic.h>

typedef struct h2_mobile_ios_app_context {
  h2_ios_platform_t *host;
  h2_runtime_t *runtime;
  atomic_bool stop;
  pthread_t thread;
  int thread_started;
} h2_mobile_ios_app_context_t;

static int h2_mobile_ios_should_stop(void *user) {
  h2_mobile_ios_app_context_t *context = user;
  return context == NULL ||
         atomic_load_explicit(&context->stop, memory_order_acquire);
}

static h2_pal_result_t
h2_mobile_ios_read_pointer(void *user, h2_mobile_pointer_state_t *out_state) {
  if (out_state == NULL) {
    return H2_PAL_ERR_INVALID_ARG;
  }
  return h2_ios_platform_read_pointer(user, &out_state->x, &out_state->y,
                                      &out_state->pressed);
}

static void *h2_mobile_ios_app_thread(void *user) {
  @autoreleasepool {
    h2_mobile_ios_app_context_t *context = user;
    const h2_mobile_app_config_t config = {
        .platform = H2_MOBILE_PLATFORM_IOS,
        .read_pointer = h2_mobile_ios_read_pointer,
        .pointer_user = context->host,
        .should_stop = h2_mobile_ios_should_stop,
        .stop_user = context,
    };
    const h2_pal_result_t result = h2_mobile_app_run(context->runtime, &config);
    NSLog(@"Firmwares tap-reset smoke App exited with result %d", result);
  }
  return NULL;
}

@interface H2MobileViewController : UIViewController
- (void)stopPortableApp;
@end

@implementation H2MobileViewController {
  h2_mobile_ios_app_context_t _context;
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = [UIColor colorWithRed:0.02
                                              green:0.03
                                               blue:0.07
                                              alpha:1.0];

  const h2_ios_platform_config_t platform_config = {
      .display_width = H2_MOBILE_APP_WIDTH,
      .display_height = H2_MOBILE_APP_HEIGHT,
  };
  _context.host = h2_ios_platform_create(&platform_config);
  if (_context.host == NULL) {
    NSLog(@"Firmwares iOS platform initialization failed");
    return;
  }
  UIView *surface = h2_ios_platform_view(_context.host);
  surface.translatesAutoresizingMaskIntoConstraints = NO;
  [self.view addSubview:surface];
  [NSLayoutConstraint activateConstraints:@[
    [surface.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
    [surface.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
    [surface.topAnchor constraintEqualToAnchor:self.view.topAnchor],
    [surface.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor],
  ]];

  h2_runtime_config_t runtime_config = h2_smoke_host_runtime_config(
      "iphone", "ios", "simulator", h2_ios_platform_mem_api(),
      h2_ios_platform_time_api(), h2_ios_platform_queue_api(),
      h2_ios_platform_display_api(_context.host));
  const h2_pal_result_t runtime_result =
      h2_runtime_init(&runtime_config, &_context.runtime);
  if (runtime_result != H2_PAL_OK) {
    NSLog(@"Firmwares Runtime initialization failed with result %d",
          runtime_result);
    h2_ios_platform_destroy(_context.host);
    _context.host = NULL;
    return;
  }
  atomic_init(&_context.stop, false);
  const int thread_result = pthread_create(&_context.thread, NULL,
                                           h2_mobile_ios_app_thread, &_context);
  if (thread_result == 0) {
    _context.thread_started = 1;
  } else {
    NSLog(@"Firmwares App thread creation failed with result %d",
          thread_result);
    [self stopPortableApp];
  }
}

- (void)stopPortableApp {
  if (_context.thread_started) {
    atomic_store_explicit(&_context.stop, true, memory_order_release);
    (void)pthread_join(_context.thread, NULL);
    _context.thread_started = 0;
  }
  if (_context.runtime != NULL) {
    h2_runtime_deinit(_context.runtime);
    _context.runtime = NULL;
  }
  if (_context.host != NULL) {
    h2_ios_platform_destroy(_context.host);
    _context.host = NULL;
  }
}

- (void)dealloc {
  [self stopPortableApp];
}

@end

@interface H2MobileSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) H2MobileViewController *controller;
@end

@implementation H2MobileSceneDelegate

- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)connectionOptions {
  (void)session;
  (void)connectionOptions;
  if (![scene isKindOfClass:UIWindowScene.class]) {
    return;
  }
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.controller = [[H2MobileViewController alloc] init];
  self.window.rootViewController = self.controller;
  [self.window makeKeyAndVisible];
}

- (void)sceneDidDisconnect:(UIScene *)scene {
  (void)scene;
  [self.controller stopPortableApp];
}

@end

@interface H2MobileAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation H2MobileAppDelegate

- (BOOL)application:(UIApplication *)application
    didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
  (void)application;
  (void)launchOptions;
  return YES;
}

- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)connectingSceneSession
                                   options:(UISceneConnectionOptions *)options {
  (void)application;
  (void)options;
  UISceneConfiguration *configuration = [[UISceneConfiguration alloc]
      initWithName:@"Default Configuration"
       sessionRole:connectingSceneSession.role];
  configuration.delegateClass = H2MobileSceneDelegate.class;
  return configuration;
}

@end

int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil,
                             NSStringFromClass(H2MobileAppDelegate.class));
  }
}
