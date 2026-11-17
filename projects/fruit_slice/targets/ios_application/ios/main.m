#include "h2_fruit_slice.h"
#include "h2_fruit_slice_native_host.h"
#include "h2_ios_platform.h"

#import <UIKit/UIKit.h>
#include <stdatomic.h>

typedef struct fruit_slice_lifecycle {
  atomic_bool paused;
  atomic_bool stopped;
} fruit_slice_lifecycle_t;

/* This callback runs only on the game worker. Never wait for it on UIKit's
 * queue: Display teardown itself needs the main queue to detach its view. */
static bool fruit_slice_should_exit(void *user) {
  fruit_slice_lifecycle_t *lifecycle = user;
  while (!atomic_load(&lifecycle->stopped)) {
    if (!atomic_load(&lifecycle->paused)) return false;
    [NSThread sleepForTimeInterval:0.05];
  }
  return true;
}

@interface FruitSliceViewController : UIViewController {
  fruit_slice_lifecycle_t _lifecycle;
  h2_ios_platform_t *_platform;
}
- (void)setGamePaused:(BOOL)paused;
- (void)stopGame;
- (void)runGameWithPlatform:(h2_ios_platform_t *)platform;
@end

@implementation FruitSliceViewController
- (instancetype)init {
  self = [super initWithNibName:nil bundle:nil];
  if (self) {
    atomic_init(&_lifecycle.paused, true);
    atomic_init(&_lifecycle.stopped, false);
  }
  return self;
}

- (BOOL)prefersStatusBarHidden {
  return YES;
}

- (BOOL)prefersHomeIndicatorAutoHidden {
  return YES;
}

- (void)setGamePaused:(BOOL)paused {
  atomic_store(&_lifecycle.paused, paused);
}

- (void)stopGame {
  atomic_store(&_lifecycle.stopped, true);
}

- (void)viewDidLoad {
  [super viewDidLoad];
  self.view.backgroundColor = UIColor.blackColor;
  const h2_ios_platform_config_t config = {.display_width = 480, .display_height = 800};
  _platform = h2_ios_platform_create(&config);
  if (!_platform) {
    NSLog(@"Fruit Slice: display creation failed");
    return;
  }
  UIView *surface = h2_ios_platform_view(_platform);
  /* The provider aspect-fits 480x800 and uses the same rect for pointer input. */
  surface.frame = self.view.bounds;
  surface.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
  [self.view addSubview:surface];

  h2_ios_platform_t *platform = _platform;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    @autoreleasepool {
      /* The block retains this controller, its atomic flags and the borrowed
       * surface owner until game and Runtime cleanup have both completed. */
      [self runGameWithPlatform:platform];
      dispatch_async(dispatch_get_main_queue(), ^{
        h2_ios_platform_destroy(platform);
        self->_platform = NULL;
      });
    }
  });
}

- (void)runGameWithPlatform:(h2_ios_platform_t *)platform {
  if (atomic_load(&_lifecycle.stopped)) return;

  NSFileManager *files = NSFileManager.defaultManager;
  NSError *error = nil;
  NSURL *support = [files URLForDirectory:NSApplicationSupportDirectory
                                 inDomain:NSUserDomainMask
                        appropriateForURL:nil
                                   create:YES
                                    error:&error];
  NSURL *directory = [support URLByAppendingPathComponent:@"FruitSlice" isDirectory:YES];
  if (!directory || ![files createDirectoryAtURL:directory
                        withIntermediateDirectories:YES
                                         attributes:nil
                                              error:&error]) {
    NSLog(@"Fruit Slice: Application Support directory failed: %@", error);
    return;
  }
  h2_ios_storage_t *storage = NULL;
  h2_pal_result_t rc = h2_ios_storage_create(directory.fileSystemRepresentation, "/data", &storage);
  if (rc != H2_PAL_OK) {
    NSLog(@"Fruit Slice: storage creation failed (%d)", rc);
    return;
  }

  h2_fruit_slice_pointer_t pointer;
  rc = h2_fruit_slice_pointer_init(&pointer, 480, 800, h2_ios_platform_read_pointer, platform);
  h2_runtime_t *runtime = NULL;
  if (rc == H2_PAL_OK) {
    h2_runtime_config_t cfg = h2_fruit_slice_native_defaults();
    cfg.board = "ios";
    cfg.target = "fruit_slice";
    cfg.chip = "apple";
    cfg.firmware_info = h2_ios_platform_firmware_info_api();
    cfg.mem = h2_ios_platform_mem_api();
    cfg.time = h2_ios_platform_time_api();
    cfg.task = h2_ios_platform_task_api();
    cfg.queue = h2_ios_platform_queue_api();
    cfg.sync = h2_ios_platform_sync_api();
    cfg.timer = h2_ios_platform_timer_api();
    cfg.log = h2_ios_platform_log_api();
    cfg.system_event = h2_ios_system_event_api();
    cfg.display = h2_ios_platform_display_api(platform);
    cfg.touch = h2_fruit_slice_pointer_touch(&pointer);
    cfg.fs = h2_ios_storage_fs_api(storage);
    cfg.pref = h2_ios_storage_pref_api(storage);
    rc = h2_runtime_init(&cfg, &runtime);
  }
  if (rc == H2_PAL_OK) {
    const h2_fruit_slice_config_t config = {
        .should_exit = fruit_slice_should_exit,
        .user = &_lifecycle,
    };
    rc = h2_fruit_slice_run(runtime, &config);
  }
  if (rc != H2_PAL_OK) NSLog(@"Fruit Slice: game failed (%d)", rc);

  /* A failed join retains Runtime borrows. Keep the pointer adapter, flags,
   * storage and platform alive and retry on this worker, never on UIKit. */
  while (runtime) {
    h2_pal_result_t cleanup = h2_runtime_destroy(&runtime);
    if (cleanup != H2_PAL_OK) NSLog(@"Fruit Slice: Runtime cleanup (%d)", cleanup);
    if (runtime) [NSThread sleepForTimeInterval:0.05];
  }
  h2_ios_storage_destroy(storage);
  /* Core is process-wide; another scene may already be connecting. Its
   * lifetime must not be ended by this scene's asynchronous cleanup. */
}
@end

@interface FruitSliceSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@property(nonatomic, strong) FruitSliceViewController *game;
@end

@implementation FruitSliceSceneDelegate
- (void)scene:(UIScene *)scene
    willConnectToSession:(UISceneSession *)session
                 options:(UISceneConnectionOptions *)options {
  (void)session;
  (void)options;
  if (![scene isKindOfClass:UIWindowScene.class]) return;
  self.game = [FruitSliceViewController new];
  self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController = self.game;
  [self.window makeKeyAndVisible];
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
  (void)scene;
  [self.game setGamePaused:NO];
}

- (void)sceneWillResignActive:(UIScene *)scene {
  (void)scene;
  [self.game setGamePaused:YES];
}

- (void)sceneDidEnterBackground:(UIScene *)scene {
  (void)scene;
  [self.game setGamePaused:YES];
}

- (void)sceneDidDisconnect:(UIScene *)scene {
  (void)scene;
  [self.game stopGame];
  self.window = nil;
  self.game = nil;
}
@end

@interface FruitSliceAppDelegate : UIResponder <UIApplicationDelegate>
@end

@implementation FruitSliceAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)application
    configurationForConnectingSceneSession:(UISceneSession *)session
                                   options:(UISceneConnectionOptions *)options {
  (void)application;
  (void)options;
  UISceneConfiguration *config = [[UISceneConfiguration alloc] initWithName:@"Fruit Slice"
                                                                sessionRole:session.role];
  config.delegateClass = FruitSliceSceneDelegate.class;
  return config;
}
@end

int main(int argc, char **argv) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, NSStringFromClass(FruitSliceAppDelegate.class));
  }
}
