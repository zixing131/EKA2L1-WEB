#import <Foundation/Foundation.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#import <UIKit/UIKit.h>

#include "sensor_ios.h"
#include <drivers/sensor/backend/ios/controller_motion.h>
#include <drivers/hwrm/backend/vibration_ios.h>

#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <thread>

@interface TestHapticPlayer : NSObject
@property(nonatomic) BOOL loopEnabled;
@property(nonatomic) NSTimeInterval loopEnd;
@property(nonatomic) NSUInteger starts;
@property(nonatomic) NSUInteger stops;
- (BOOL)startAtTime:(NSTimeInterval)time error:(NSError **)error;
- (BOOL)stopAtTime:(NSTimeInterval)time error:(NSError **)error;
@end
@implementation TestHapticPlayer
- (BOOL)startAtTime:(NSTimeInterval)time error:(NSError **)error { ++_starts; return YES; }
- (BOOL)stopAtTime:(NSTimeInterval)time error:(NSError **)error { ++_stops; return YES; }
@end

@interface TestHapticEngine : NSObject
@property(nonatomic) BOOL playsHapticsOnly;
@property(nonatomic) BOOL failsToStart;
@property(nonatomic) BOOL throwsOnStart;
@property(nonatomic, strong) NSMutableArray<TestHapticPlayer *> *players;
@property(nonatomic, strong) CHHapticPattern *pattern;
- (BOOL)startAndReturnError:(NSError **)error;
- (void)stopWithCompletionHandler:(void (^)(NSError *))handler;
- (id<CHHapticAdvancedPatternPlayer>)createAdvancedPlayerWithPattern:(CHHapticPattern *)pattern error:(NSError **)error;
@end
@implementation TestHapticEngine
- (instancetype)init {
    if ((self = [super init])) _players = [NSMutableArray array];
    return self;
}
- (BOOL)startAndReturnError:(NSError **)error {
    assert(error != NULL);
    if (_throwsOnStart) [NSException raise:@"CHHapticTestException" format:@"engine start failed"];
    return !_failsToStart;
}
- (void)stopWithCompletionHandler:(void (^)(NSError *))handler {
    for (TestHapticPlayer *player in _players) [player stopAtTime:0 error:nil];
    if (handler) handler(nil);
}
- (id<CHHapticAdvancedPatternPlayer>)createAdvancedPlayerWithPattern:(CHHapticPattern *)pattern error:(NSError **)error {
    assert(error != NULL);
    _pattern = pattern;
    TestHapticPlayer *player = [TestHapticPlayer new];
    [_players addObject:player];
    return (id<CHHapticAdvancedPatternPlayer>)player;
}
@end

@interface TestHaptics : NSObject
@property(nonatomic, strong) NSMutableArray<TestHapticEngine *> *engines;
- (CHHapticEngine *)createEngineWithLocality:(GCHapticsLocality)locality;
@end
@implementation TestHaptics
- (instancetype)init {
    if ((self = [super init])) _engines = [NSMutableArray array];
    return self;
}
- (CHHapticEngine *)createEngineWithLocality:(GCHapticsLocality)locality {
    assert([locality isEqualToString:GCHapticsLocalityDefault]);
    TestHapticEngine *engine = [TestHapticEngine new];
    [_engines addObject:engine];
    return (CHHapticEngine *)engine;
}
@end

@interface TestFeedbackController : NSObject
@property(nonatomic, strong) TestHaptics *haptics;
@end
@implementation TestFeedbackController
@end

@interface TestManualMotion : GCMotion
@property(nonatomic) BOOL testActive;
@end
@implementation TestManualMotion
- (BOOL)sensorsRequireManualActivation { return YES; }
- (BOOL)sensorsActive { return _testActive; }
- (void)setSensorsActive:(BOOL)active { _testActive = active; }
- (BOOL)hasGravityAndUserAcceleration { return YES; }
- (GCAcceleration)acceleration { return {0.5, 0, -1}; }
- (GCAcceleration)gravity { return {0, 0, -1}; }
@end

@interface TestMotionController : NSObject
@property(nonatomic, strong) TestManualMotion *motion;
@end
@implementation TestMotionController
@end

using namespace eka2l1::drivers;
using namespace std::chrono_literals;

template <typename Packet>
Packet read_packet(sensor &channel) {
    auto result = std::make_shared<std::promise<Packet>>();
    auto future = result->get_future();
    channel.receive_data([result](std::vector<std::uint8_t> &data, std::size_t count) {
        assert(count > 0 && data.size() >= sizeof(Packet));
        Packet packet;
        std::memcpy(&packet, data.data(), sizeof(packet));
        result->set_value(packet);
    });
    assert(future.wait_for(2s) == std::future_status::ready);
    return future.get();
}

static void test_motion() {
    GCController *controller = [GCController controllerWithExtendedGamepad];
    [controller.motion setAcceleration:GCAcceleration{0, -1, 0}];
    sensor_driver_ios driver;
    set_controller_motion_source(&driver, (__bridge void *)controller);
    assert(driver.accelerometer_available());
    assert(driver.queries_active_sensor({}).size() == 2);
    auto acceleration = driver.new_sensor_controller(1);
    auto rotation = driver.new_sensor_controller(2);
    assert(acceleration && rotation);
    assert(acceleration->listen_for_data(1, 1, 0));
    assert(rotation->listen_for_data(1, 1, 0));
    auto a = read_packet<sensor_accelerometer_axis_data>(*acceleration);
    assert(a.axis_x_ == 0 && a.axis_y_ > 60 && a.axis_z_ == 0);
    auto r = read_packet<sensor_rotation_data>(*rotation);
    assert(r.x_ == 0 && r.y_ == -1 && r.z_ == 0);

    driver.set_motion_rotation(90);
    a = read_packet<sensor_accelerometer_axis_data>(*acceleration);
    assert(a.axis_x_ == 0 && a.axis_y_ > 60);
    set_controller_motion_rotation(&driver, 90);
    a = read_packet<sensor_accelerometer_axis_data>(*acceleration);
    assert(a.axis_x_ > 60 && a.axis_y_ == 0);
    r = read_packet<sensor_rotation_data>(*rotation);
    assert(r.x_ == -1 && r.y_ == 90 && r.z_ == 270);

    assert(driver.pause());
    auto result = std::make_shared<std::promise<bool>>();
    auto future = result->get_future();
    acceleration->receive_data([result](auto &, auto) { result->set_value(true); });
    assert(future.wait_for(100ms) == std::future_status::timeout);
    assert(driver.resume());
    assert(future.wait_for(2s) == std::future_status::ready);
    assert(future.get());

    assert(driver.pause());
    GCController *replacement = [GCController controllerWithExtendedGamepad];
    [replacement.motion setAcceleration:GCAcceleration{0, 0, -1}];
    set_controller_motion_source(&driver, (__bridge void *)replacement);
    assert(driver.resume());
    a = read_packet<sensor_accelerometer_axis_data>(*acceleration);
    assert(a.axis_x_ == 0 && a.axis_y_ == 0 && a.axis_z_ > 60);
    assert(acceleration->cancel_data_listening());
    assert(rotation->cancel_data_listening());
    set_controller_motion_source(&driver, nullptr);

    TestMotionController *manual = [TestMotionController new];
    manual.motion = [TestManualMotion new];
    set_controller_motion_source(&driver, (__bridge void *)manual);
    set_controller_motion_rotation(&driver, 0);
    assert(!manual.motion.sensorsActive);
    assert(acceleration->listen_for_data(1, 1, 0));
    assert(rotation->listen_for_data(1, 1, 0));
    assert(manual.motion.sensorsActive);
    a = read_packet<sensor_accelerometer_axis_data>(*acceleration);
    r = read_packet<sensor_rotation_data>(*rotation);
    assert(a.axis_x_ < -30 && a.axis_z_ > 60);
    assert(r.x_ == 90 && r.y_ == 180 && r.z_ == -1);
    assert(driver.pause() && !manual.motion.sensorsActive);
    assert(driver.resume() && manual.motion.sensorsActive);
    assert(acceleration->cancel_data_listening() && manual.motion.sensorsActive);
    assert(rotation->cancel_data_listening() && !manual.motion.sensorsActive);
    set_controller_motion_source(&driver, nullptr);

    for (int i = 0; i < 100; ++i) {
        auto transientDriver = std::make_unique<sensor_driver_ios>();
        set_controller_motion_source(transientDriver.get(), (__bridge void *)controller);
        auto channel = transientDriver->new_sensor_controller(1);
        assert(channel->listen_for_data(1, 1, 0));
        channel->receive_data([](auto &, auto) {});
        channel.reset();
        transientDriver.reset();
    }
    std::this_thread::sleep_for(100ms);
}

static void test_haptics() {
    using namespace eka2l1::drivers::hwrm;
    TestFeedbackController *first = [TestFeedbackController new];
    first.haptics = [TestHaptics new];
    TestFeedbackController *second = [TestFeedbackController new];
    second.haptics = [TestHaptics new];
    set_vibration_suspended(false);
    set_controller_haptic_source((__bridge void *)first);
    auto vibrator = std::make_unique<vibrator_ios>();
    vibrator->vibrate(250, 80);
    assert(first.haptics.engines.count == 1);
    TestHapticEngine *engine = first.haptics.engines.lastObject;
    TestHapticPlayer *player = engine.players.lastObject;
    assert(player.starts == 1 && !player.loopEnabled);
    assert(std::abs(engine.pattern.duration - 0.25) < 0.001);
    vibrator->vibrate(0, -80);
    assert(player.stops > 0);
    player = engine.players.lastObject;
    assert(player.loopEnabled && player.loopEnd == 1 && player.starts == 1);
    vibrator->stop_vibrate();
    assert(player.stops > 0);

    vibrator->vibrate(100, 50);
    player = engine.players.lastObject;
    set_controller_haptic_source((__bridge void *)second);
    assert(player.stops > 0);
    vibrator->vibrate(100, 50);
    assert(second.haptics.engines.count == 1);
    engine = second.haptics.engines.lastObject;
    player = engine.players.lastObject;
    set_vibration_suspended(true);
    assert(player.stops > 0);
    vibrator->vibrate(100, 50);
    assert(engine.players.count == 1);
    set_vibration_suspended(false);
    vibrator->vibrate(100, 50);
    assert(second.haptics.engines.count == 2);
    engine = second.haptics.engines.lastObject;
    player = engine.players.lastObject;
    engine.failsToStart = YES;
    vibrator->vibrate(100, 50);
    assert(player.stops > 0 && engine.players.count == 1);
    engine.failsToStart = NO;
    vibrator->vibrate(100, 50);
    player = engine.players.lastObject;
    assert(player.starts == 1);

    engine.throwsOnStart = YES;
    vibrator->vibrate(100, 50);
    assert(player.stops > 0 && second.haptics.engines.count == 2);
    engine.throwsOnStart = NO;
    vibrator->vibrate(100, 50);
    assert(second.haptics.engines.count == 3);
    engine = second.haptics.engines.lastObject;
    player = engine.players.lastObject;
    assert(player.starts == 1);
    vibrator.reset();
    assert(player.stops > 0);
    set_controller_haptic_source(nullptr);
}

@interface ControllerFeedbackTestDelegate : UIResponder <UIApplicationDelegate>
@end
@implementation ControllerFeedbackTestDelegate
- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)options {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        test_motion();
        test_haptics();
        NSString *result = @"PASS: controller motion channels, axis rotation, pause/resume, source switch, teardown and haptic routing/lifetime\n";
        [result writeToFile:[NSHomeDirectory() stringByAppendingPathComponent:@"Documents/result.txt"]
                 atomically:YES encoding:NSUTF8StringEncoding error:nil];
    });
    return YES;
}
@end

int main(int argc, char **argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(ControllerFeedbackTestDelegate.class));
    }
}
