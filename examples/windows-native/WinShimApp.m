#include <UIKit/UIKit.h>
#include <string.h>

@interface WinShimViewController : UIViewController
@end

@implementation WinShimViewController
- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = [UIColor systemBackgroundColor];
    UILabel *label = [[UILabel alloc] initWithFrame:self.view.bounds];
    label.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    label.textAlignment = NSTextAlignmentCenter;
    label.numberOfLines = 0;
    label.text = @"Windows C entrypoint\ncompiled to arm64 iOS by clang + LLD";
    [self.view addSubview:label];
}
@end

@interface WinShimAppDelegate : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation WinShimAppDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    (void)application; (void)options;
    self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
    self.window.rootViewController = [WinShimViewController new];
    [self.window makeKeyAndVisible];
    return YES;
}
@end

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    @autoreleasepool {
        return UIApplicationMain(0, NULL, nil, NSStringFromClass(WinShimAppDelegate.class));
    }
}
