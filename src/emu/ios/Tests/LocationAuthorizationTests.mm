#import <CoreLocation/CoreLocation.h>
#import <UIKit/UIKit.h>
#import <objc/runtime.h>

#include <drivers/location/location.h>
#include "../../drivers/src/location/backend/ios/location_ios.h"

#include <cassert>
#include <memory>

static UIApplicationState application_state = UIApplicationStateActive;
static CLAuthorizationStatus authorization_status = kCLAuthorizationStatusNotDetermined;
static unsigned manager_count, prompts, starts, stops, fixes;

@interface TestLocationManager : NSObject
@property(nonatomic, weak) id<CLLocationManagerDelegate> delegate;
@property(nonatomic) CLLocationAccuracy desiredAccuracy;
@property(nonatomic) CLLocationDistance distanceFilter;
@property(nonatomic, readonly) CLAuthorizationStatus authorizationStatus;
- (void)requestWhenInUseAuthorization;
- (void)startUpdatingLocation;
- (void)stopUpdatingLocation;
@end

static TestLocationManager *manager;

@implementation TestLocationManager
- (instancetype)init {
    assert(NSThread.isMainThread);
    if ((self = [super init])) {
        ++manager_count;
        manager = self;
    }
    return self;
}
- (CLAuthorizationStatus)authorizationStatus { return authorization_status; }
- (void)requestWhenInUseAuthorization {
    assert(NSThread.isMainThread && application_state == UIApplicationStateActive);
    assert(authorization_status == kCLAuthorizationStatusNotDetermined);
    ++prompts;
}
- (void)startUpdatingLocation {
    assert(NSThread.isMainThread);
    assert(authorization_status == kCLAuthorizationStatusAuthorizedWhenInUse
        || authorization_status == kCLAuthorizationStatusAuthorizedAlways);
    ++starts;
}
- (void)stopUpdatingLocation {
    assert(NSThread.isMainThread);
    ++stops;
}
@end

static id allocate_manager(id cls, SEL selector) __attribute__((ns_returns_retained));
static id allocate_manager(id cls, SEL selector) {
    return [TestLocationManager alloc];
}

static void drain_main_queue() {
    dispatch_sync(dispatch_get_main_queue(), ^{});
}

static void change_authorization(CLAuthorizationStatus status) {
    authorization_status = status;
    [manager.delegate locationManagerDidChangeAuthorization:(CLLocationManager *)manager];
}

static void become_active() {
    application_state = UIApplicationStateActive;
    [[NSNotificationCenter defaultCenter] postNotificationName:UIApplicationDidBecomeActiveNotification object:nil];
}

static void deliver(id<CLLocationManagerDelegate> delegate) {
    CLLocation *location = [[CLLocation alloc] initWithLatitude:1.25 longitude:103.75];
    [delegate locationManager:(CLLocationManager *)manager didUpdateLocations:@[location]];
}

static void receive_fix(const eka2l1::drivers::location_fix &fix) {
    assert(fix.latitude_ == 1.25 && fix.longitude_ == 103.75);
    ++fixes;
}

static void test_authorization() {
    using eka2l1::drivers::location_driver_ios;
    auto driver = std::make_unique<location_driver_ios>();
    location_driver_ios *raw = driver.get();

    // A guest can close its positioner before the main queue processes start().
    dispatch_sync(dispatch_get_main_queue(), ^{
        raw->start(receive_fix);
        raw->stop();
    });
    drain_main_queue();
    assert(manager_count == 0 && prompts == 0 && starts == 0 && stops == 0);

    dispatch_sync(dispatch_get_main_queue(), ^{ application_state = UIApplicationStateInactive; });
    driver->start(receive_fix);
    drain_main_queue();
    assert(manager_count == 1 && prompts == 0 && starts == 0);

    dispatch_sync(dispatch_get_main_queue(), ^{
        become_active();
        become_active();
        change_authorization(kCLAuthorizationStatusNotDetermined);
    });
    assert(prompts == 1 && starts == 0 && stops == 0);

    // Another alert can win presentation even if we requested while still active.
    dispatch_sync(dispatch_get_main_queue(), ^{
        application_state = UIApplicationStateInactive;
        [[NSNotificationCenter defaultCenter] postNotificationName:UIApplicationWillResignActiveNotification object:nil];
        change_authorization(kCLAuthorizationStatusNotDetermined);
        assert(prompts == 1);
        become_active();
        become_active();
    });
    assert(prompts == 2 && starts == 0 && stops == 0);

    dispatch_sync(dispatch_get_main_queue(), ^{
        change_authorization(kCLAuthorizationStatusDenied);
        deliver(manager.delegate);
        become_active();
    });
    assert(prompts == 2 && starts == 0 && stops == 0 && fixes == 0);
    driver->stop();
    driver->start(receive_fix);
    drain_main_queue();
    assert(prompts == 2 && starts == 0 && stops == 0);

    dispatch_sync(dispatch_get_main_queue(), ^{
        change_authorization(kCLAuthorizationStatusRestricted);
        become_active();
        application_state = UIApplicationStateInactive;
        change_authorization(kCLAuthorizationStatusAuthorizedWhenInUse);
        assert(starts == 0);
        become_active();
        change_authorization(kCLAuthorizationStatusAuthorizedWhenInUse);
        deliver(manager.delegate);
    });
    assert(prompts == 2 && starts == 1 && fixes == 1);

    dispatch_sync(dispatch_get_main_queue(), ^{
        raw->stop();
        deliver(manager.delegate);
        change_authorization(kCLAuthorizationStatusAuthorizedAlways);
    });
    drain_main_queue();
    driver->stop();
    drain_main_queue();
    assert(starts == 1 && stops == 1 && fixes == 1);

    driver->start(receive_fix);
    drain_main_queue();
    assert(manager_count == 1 && starts == 2 && prompts == 2);
    dispatch_sync(dispatch_get_main_queue(), ^{
        application_state = UIApplicationStateBackground;
        change_authorization(kCLAuthorizationStatusNotDetermined);
    });
    assert(stops == 2 && prompts == 2);
    dispatch_sync(dispatch_get_main_queue(), ^{
        become_active();
        change_authorization(kCLAuthorizationStatusAuthorizedWhenInUse);
    });
    assert(starts == 3 && prompts == 3);

    __block id<CLLocationManagerDelegate> queued_delegate;
    __block TestLocationManager *queued_manager;
    __block __weak TestLocationManager *released_manager;
    dispatch_sync(dispatch_get_main_queue(), ^{
        queued_delegate = manager.delegate;
        queued_manager = manager;
        released_manager = manager;
        manager = nil;
    });
    driver.reset();
    drain_main_queue();
    assert(stops == 3);
    dispatch_sync(dispatch_get_main_queue(), ^{
        [queued_delegate locationManagerDidChangeAuthorization:(CLLocationManager *)queued_manager];
        deliver(queued_delegate);
        become_active();
        queued_delegate = nil;
        queued_manager = nil;
        assert(released_manager == nil);
    });
    assert(starts == 3 && stops == 3 && fixes == 1);

    dispatch_sync(dispatch_get_main_queue(), ^{ authorization_status = kCLAuthorizationStatusNotDetermined; });
    driver = std::make_unique<location_driver_ios>();
    driver->start(receive_fix);
    drain_main_queue();
    assert(prompts == 4 && starts == 3);
    dispatch_sync(dispatch_get_main_queue(), ^{ queued_delegate = manager.delegate; });
    driver.reset();
    drain_main_queue();
    dispatch_sync(dispatch_get_main_queue(), ^{
        authorization_status = kCLAuthorizationStatusAuthorizedWhenInUse;
        [queued_delegate locationManagerDidChangeAuthorization:(CLLocationManager *)manager];
        deliver(queued_delegate);
        manager = nil;
        queued_delegate = nil;
    });
    assert(stops == 3 && starts == 3 && fixes == 1);
}

@interface LocationAuthorizationTestDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation LocationAuthorizationTestDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    return YES;
}
@end

@interface LocationAuthorizationSceneDelegate : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow *window;
@end
@implementation LocationAuthorizationSceneDelegate
- (void)scene:(UIScene *)scene willConnectToSession:(UISceneSession *)session options:(UISceneConnectionOptions *)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene *)scene];
    self.window.rootViewController = [UIViewController new];
    [self.window makeKeyAndVisible];
}

- (void)sceneDidBecomeActive:(UIScene *)scene {
    static bool tested = false;
    if (tested) return;
    tested = true;
    Method allocation = class_getClassMethod(CLLocationManager.class, @selector(alloc));
    class_replaceMethod(object_getClass(CLLocationManager.class), @selector(alloc),
        reinterpret_cast<IMP>(allocate_manager), method_getTypeEncoding(allocation));
    Method state = class_getInstanceMethod(UIApplication.class, @selector(applicationState));
    method_setImplementation(state, imp_implementationWithBlock(^UIApplicationState(id app) { return application_state; }));
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        test_authorization();
        NSString *result = @"PASS: foreground authorization, denial, grant, revocation, cancellation and delegate lifetime\n";
        [result writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/result.txt"]
                 atomically:YES encoding:NSUTF8StringEncoding error:nil];
    });
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(LocationAuthorizationTestDelegate.class));
    }
}
