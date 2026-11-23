import Foundation
import GameController

@main
enum ControllerPointerTests {
    static func main() {
        let controller = GCController.withExtendedGamepad()
        let gamepad = controller.extendedGamepad!
        var features = ControllerFeatures()
        var state = ControllerPointerState()

        func actions() -> Set<ControllerPointerAction> {
            let tokens = Set(HostButton.allCases
                .filter { $0.isPressed(on: gamepad, threshold: 0.45) }.map(\.rawValue))
            return features.actions(for: tokens)
        }

        gamepad.rightThumbstickButton!.setValue(1)
        precondition(state.update(actions: actions()).isEmpty && state.isVisible)
        precondition(state.update(actions: actions()).isEmpty && state.isVisible)
        gamepad.rightThumbstickButton!.setValue(0)
        precondition(state.update(actions: actions()).isEmpty)
        gamepad.buttonA.setValue(1)
        precondition(state.update(actions: actions()) == [.began])
        precondition(state.update(actions: actions()).isEmpty)

        gamepad.leftThumbstick.setValueForXAxis(0, yAxis: 0.44)
        _ = state.update(actions: actions())
        precondition(state.move(elapsed: 1.0 / 60, size: CGSize(width: 640, height: 360)).isEmpty)
        gamepad.leftThumbstick.setValueForXAxis(0, yAxis: 0.46)
        _ = state.update(actions: actions())
        precondition(state.move(elapsed: 1.0 / 60, size: CGSize(width: 640, height: 360)) == [.moved])
        precondition(state.position.y < 0.5)
        gamepad.buttonA.setValue(0)
        precondition(state.update(actions: actions()) == [.ended])
        precondition(state.move(elapsed: 1.0 / 60, size: CGSize(width: 640, height: 360)).isEmpty)

        gamepad.buttonA.setValue(1)
        precondition(state.update(actions: actions()) == [.began])
        gamepad.rightThumbstickButton!.setValue(1)
        precondition(state.update(actions: actions()) == [.cancelled] && !state.isVisible)
        gamepad.rightThumbstickButton!.setValue(0)
        _ = state.update(actions: actions())
        gamepad.rightThumbstickButton!.setValue(1)
        precondition(state.update(actions: actions()).isEmpty && state.isVisible)
        precondition(!state.isTouching)
        gamepad.rightThumbstickButton!.setValue(0)
        gamepad.buttonA.setValue(0)
        _ = state.update(actions: actions())
        gamepad.buttonA.setValue(1)
        precondition(state.update(actions: actions()) == [.began])
        precondition(state.cancelTouch() == [.cancelled])
        precondition(state.update(actions: actions()).isEmpty && !state.isTouching)

        gamepad.buttonA.setValue(0)
        _ = state.update(actions: actions())
        gamepad.buttonA.setValue(1)
        precondition(state.update(actions: actions()) == [.began])
        precondition(state.reset() == [.cancelled] && !state.isVisible)
        precondition(state.reset().isEmpty)
        state.prime(actions: [.toggle, .touch])
        precondition(state.update(actions: [.toggle, .touch]).isEmpty && !state.isVisible)
        _ = state.update(actions: [])
        _ = state.update(actions: [.toggle])
        precondition(state.isVisible)

        var horizontal = ControllerPointerState()
        var diagonal = ControllerPointerState()
        _ = horizontal.update(actions: [.toggle])
        _ = diagonal.update(actions: [.toggle])
        _ = horizontal.update(actions: [.right])
        _ = diagonal.update(actions: [.right, .down])
        let size = CGSize(width: 640, height: 360)
        _ = horizontal.move(elapsed: 1.0 / 60, size: size)
        _ = diagonal.move(elapsed: 1.0 / 60, size: size)
        let horizontalDistance = (horizontal.position.x - 0.5) * (size.width - 1)
        let diagonalDistance = hypot((diagonal.position.x - 0.5) * (size.width - 1),
                                     (diagonal.position.y - 0.5) * (size.height - 1))
        precondition(abs(horizontalDistance - diagonalDistance) < 0.0001)
        _ = diagonal.update(actions: [.up, .down, .left, .right])
        let before = diagonal.position
        _ = diagonal.move(elapsed: 1.0 / 60, size: size)
        precondition(diagonal.position == before)
        _ = diagonal.update(actions: [.right, .down])
        for _ in 0..<600 { _ = diagonal.move(elapsed: 1.0 / 60, size: size) }
        precondition(diagonal.position == CGPoint(x: 1, y: 1))
        let crop = CGRect(x: 30, y: 200, width: 640, height: 360)
        precondition(diagonal.location(in: crop) == CGPoint(x: 669, y: 559))
        let tv = CGRect(x: 0, y: 0, width: 1920, height: 1080)
        precondition(ControllerPointerState.fittedRect(size: size, in: tv) == tv)
        let portrait = ControllerPointerState.fittedRect(size: CGSize(width: 360, height: 640), in: tv)
        precondition(portrait.minX > 0 && portrait.minY == 0 && portrait.height == 1080)

        features.bind(token: HostButton.buttonY.rawValue, to: .toggle)
        precondition(features.pointerBindings[HostButton.rightThumbstickButton.rawValue] == nil)
        precondition(features.actions(for: [HostButton.buttonY.rawValue]) == [.toggle])
        features.bind(token: HostButton.dpadUp.rawValue, to: .touch)
        precondition(features.pointerBindings[HostButton.buttonA.rawValue] == nil)
        precondition(features.pointerBindings[HostButton.leftStickUp.rawValue] == .up)
        features.unbind(.left)
        features.motionEnabled = false
        features.pointerEnabled = false
        let deviceKey = "controller-tests-\(UUID().uuidString)"
        features.save(deviceKey: deviceKey)
        precondition(ControllerFeatures.load(deviceKey: deviceKey) == features)
        precondition(ControllerFeatures.load(deviceKey: "unused-\(deviceKey)") == ControllerFeatures())

        print("PASS: controller snapshots, click/drag, cancellation, held buttons, movement, letterboxing and per-device settings")
    }
}
