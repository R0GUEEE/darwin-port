// WindowProbe - the smallest thing that proves an iOS app built on Linux is a
// real app: a window, a root view controller, one label, no Apple toolchain.
#import <UIKit/UIKit.h>

@interface AppDelegate : UIResponder <UIApplicationDelegate>
@property (nonatomic, strong) UIWindow *window;
@end

@implementation AppDelegate

- (BOOL)application:(UIApplication *)app
        didFinishLaunchingWithOptions:(NSDictionary *)options {
  self.window = [[UIWindow alloc] initWithFrame:[[UIScreen mainScreen] bounds]];
  UIViewController *root = [[UIViewController alloc] init];
  root.view.backgroundColor = [UIColor systemBackgroundColor];

  UILabel *label = [[UILabel alloc] initWithFrame:CGRectZero];
  label.text = @"built on linux";
  label.font = [UIFont systemFontOfSize:28 weight:UIFontWeightSemibold];
  label.textAlignment = NSTextAlignmentCenter;
  [label sizeToFit];
  label.center = root.view.center;
  label.autoresizingMask = UIViewAutoresizingFlexibleTopMargin |
                           UIViewAutoresizingFlexibleBottomMargin;
  [root.view addSubview:label];

  self.window.rootViewController = root;
  [self.window makeKeyAndVisible];
  return YES;
}

@end

int main(int argc, char *argv[]) {
  @autoreleasepool {
    return UIApplicationMain(argc, argv, nil, @"AppDelegate");
  }
}
