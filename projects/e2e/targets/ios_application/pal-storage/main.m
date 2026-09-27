#import <UIKit/UIKit.h>
#include "h2_ios_platform.h"
#include "h2_mobile_app_host.h"
#include "runner.h"

static int teardown(void *owner) {
  h2_ios_storage_destroy(owner);
  return h2_ios_platform_core_shutdown();
}
@interface StorageViewController : UIViewController
@end
@implementation StorageViewController
- (void)viewDidLoad {
  [super viewDidLoad];
  UITextView *view=[[UITextView alloc] initWithFrame:self.view.bounds];
  view.autoresizingMask=UIViewAutoresizingFlexibleWidth|UIViewAutoresizingFlexibleHeight;
  view.editable=NO;view.text=@"PAL Storage E2E running…";[self.view addSubview:view];
  NSArray<NSString *> *args=NSProcessInfo.processInfo.arguments;
  NSUInteger phaseIndex=[args indexOfObject:@"--phase"],nonceIndex=[args indexOfObject:@"--nonce"];
  unsigned phase=phaseIndex!=NSNotFound && phaseIndex+1<args.count?[args[phaseIndex+1] intValue]:1;
  uint32_t nonce=nonceIndex!=NSNotFound && nonceIndex+1<args.count?(uint32_t)[args[nonceIndex+1] longLongValue]:1;
  NSString *documents=NSSearchPathForDirectoriesInDomains(NSDocumentDirectory,NSUserDomainMask,YES).firstObject;
  NSString *directory=[documents stringByAppendingPathComponent:@"pal-storage"];
  NSString *report=[documents stringByAppendingPathComponent:@"pal-storage-phase.json"];
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
    @autoreleasepool {
      h2_ios_storage_t *storage=NULL;
      int rc=h2_ios_storage_create(directory.fileSystemRepresentation,"/storage",&storage);
      if (rc==H2_PAL_OK) {
        h2_runtime_config_t config=h2_ios_app_host_config();
        config.fs=h2_ios_storage_fs_api(storage);config.pref=h2_ios_storage_pref_api(storage);
        rc=h2_storage_mobile_phase(config,phase,nonce,report.fileSystemRepresentation,teardown,storage);
      }
      NSString *text=[NSString stringWithContentsOfFile:report encoding:NSUTF8StringEncoding error:nil];
      dispatch_async(dispatch_get_main_queue(),^{view.text=[NSString stringWithFormat:@"PAL Storage phase %u: %@ (%d)\n%@",phase,rc==0?@"PASS":@"FAIL",rc,text?:@""];});
    }
  });
}
@end
@interface StorageSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic,strong) UIWindow *window;
@end
@implementation StorageSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
  (void)session;(void)options;
  self.window=[[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
  self.window.rootViewController=[StorageViewController new];[self.window makeKeyAndVisible];
}
@end
@interface StorageAppDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation StorageAppDelegate
- (UISceneConfiguration *)application:(UIApplication *)app configurationForConnectingSceneSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
  (void)app;(void)options;
  UISceneConfiguration *config=[[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
  config.delegateClass=StorageSceneDelegate.class;return config;
}
@end
int main(int argc,char **argv) {
  @autoreleasepool {return UIApplicationMain(argc,argv,nil,NSStringFromClass(StorageAppDelegate.class));}
}
