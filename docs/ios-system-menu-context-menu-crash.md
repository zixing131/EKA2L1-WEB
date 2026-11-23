# iOS system menu corrupts UIKit collection storage

Two TestFlight reports from 26.7.0 initially looked like unrelated main-thread
memory corruption:

- build 260784 crashed on iOS 18.7.9 while
  `_UIContextMenuListView` registered trait changes;
- build 260795 crashed on iOS 26.5.2 while a context menu and its rapid-click
  presentation assistant were being dismissed through `UIRemoteKeyboardWindow`.

They came from different devices and app commits, but both attempted
`objc_release(0x3)`. `0x3` is neither an Objective-C object nor a valid tagged
pointer. Exact dSYM matching also confirmed that neither failure had an EKA2L1
frame in its active call chain. Build 260795's EKA2L1 UUID
`DD8C805C-A0E7-3EE3-B9C9-D9E867905A74` maps to commit `3d063edf5d19`.

## UIKitCore reverse engineering

The closest simulator UIKitCore binaries were thinned to arm64 and inspected
with their Objective-C metadata and LLDB disassembly. Runtime breakpoints on
iOS 26.5 then validated object identities and retain counts on the non-crashing
path.

### Presentation: trait-registration set

The iOS 18 implementation of
`-[_UITraitChangeRegistry _addRegistration:forTraitTokens:]` is equivalent to:

```objc
for (id token in traitTokens) {
    NSMutableSet *set = [_registrations objectForKey:token];
    if (set) {
        [set addObject:registration];
    } else {
        set = [[NSMutableSet alloc] initWithObjects:registration, nil];
        [_registrations setObject:set forKey:token];
        [set release];
    }
}
```

The crash frame is method offset `+296`. The immediately preceding call at
`+292` releases the newly-created set after the dictionary has retained it.
The report's registers agree with this reconstruction: `x21` is the trait
registry, `x24` the current trait token, `x25` its mutable dictionary, and
`x26` the new set. On a normal iOS 26.5 run, a breakpoint at the same release
showed that the set contained the registration and had retain count 2. Thus
UIKit is not directly calling `release(3)` at that source line. Releasing the
valid set enters a nested Foundation cleanup path which eventually tail-calls
`objc_release` with `x0 = 3`.

### Dismissal: remote-keyboard options dictionary

`+[UIRemoteKeyboardWindow remoteKeyboardWindowForScreen:create:]` copies a
static defaults dictionary into an `NSMutableDictionary`, optionally inserts
one `NSNumber`, passes the options to the keyboard-window registry, and releases
the local dictionary. All values produced by this method are Objective-C
objects.

The second report reaches `-[__NSDictionaryM dealloc]`. Reconstructing
CoreFoundation's `cow_cleanup` loop from its registers is more conclusive:

- `x19` is the dictionary storage base;
- the decoded capacity is 7;
- `x22 = x19 + 0x38` is the end of the seven-key region;
- `x21 = x19 + 0x48` is the post-incremented value cursor;
- `x20 = 6` means one value was already processed.

The failing `0x3` therefore came from the second value slot at
`storage + 0x40`. It was inside the dictionary's value area, not an allocator
trailer or an Objective-C tagged value. A normal runtime capture of the same
dictionary contained four real `NSNumber` values and zeroed unused slots.

## Root cause and rejected leads

The direct root cause is corrupted or prematurely-reused Foundation collection
storage owned by UIKit's private context-menu implementation. Presentation
hits it through the trait registry; dismissal hits it through the keyboard
suppression assertion. In both cases UIKit begins with valid objects, but a
container cleanup later observes the impossible stored value `0x3`.

The exact earlier write or lost retain is not recoverable from post-mortem crash
reports. It could be a private UIKit re-entrancy/lifetime defect or prior
process-heap corruption; the reports prove the corrupted storage and the
context-menu boundary, not which private instruction first damaged it.

Several narrower theories were checked:

- **The menu is inside the render view.** It is not. It is a sibling SwiftUI
  overlay above the `UIViewControllerRepresentable`.
- **The permanent render view first responder is the common root.** It affects
  the dismissal path because `_UIRapidClickPresentationAssistant` vends and
  later clears a keyboard-suppression assertion. It cannot explain the first
  crash, which occurs while the menu list is being created and registering
  traits. Resigning it remains useful containment, but is not the root fix.
- **UIKit intentionally uses the scalar `3` as an object.** The click
  interaction does have a presentation-type enum whose value can be 3, but
  disassembly keeps that ivar scalar and sends it only to statistics code.
  The first report also used an active touch driver, so that enum was not 3
  there. It does not flow into either failing collection.
- **The dictionary cleanup merely walked beyond its allocation.** The register
  geometry places `3` in an in-capacity value slot.

## Fix

The initial containment replaced the SwiftUI `Menu` with a regular
popover/sheet, avoiding the private context-menu path entirely. Further
investigation isolated the failing setup to UIKit's type-selection interaction
on the collection view backing the menu. The iOS app now supplies the expected
private hook as a no-op:

```swift
extension UICollectionView {
    @objc func _configureTypeSelectInteractionIfNeeded() {}
}
```

With that interaction disabled, the system key can use SwiftUI `Menu` again.
The keypad and game settings remain nested menus, restoring the compact native
interaction without reintroducing the crashing collection setup. Hardware-key
capture is still released while editing the keypad layout, but no longer needs
special handling just for opening and closing the system menu.

This remains an iOS host-UI compatibility fix. It does not change guest input
handling, renderer ownership, or shared emulator behavior.

## Verification

The original popover containment passed Debug simulator inspection, two full
Release regression runs, and the Angry Birds touch suite. The restored
SwiftUI-menu implementation builds successfully and its top-level menu plus
both settings submenus were visually exercised on the iOS 26.5 simulator
without a crash. Repeated open/close cycles must additionally be exercised on a
physical device, where the original failures occurred.
