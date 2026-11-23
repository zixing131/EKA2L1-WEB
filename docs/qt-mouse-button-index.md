# Qt mouse clicks were delivered as right-button events

On macOS, Need for Speed: Shift reached its language menu but clicking the
language arrows did nothing. The mouse cursor was over the correct target.

Tracing one press and release through the Qt widget and window server ruled out
Retina scaling, letterboxing, and rotation. A click on the right arrow reached
guest coordinates (504, 202), with pointer ID zero for both events. However, the
driver button value was one: its right button, rather than its left button.

Qt encodes buttons as bit masks: left is 1, right is 2, and middle is 4. The driver
uses zero-based indices: left is 0, right is 1, and middle is 2. The widget used
`find_most_significant_bit_one`, whose Clang implementation returns the bit width,
and therefore forwarded every button one index too high. The window server
correctly turned these into secondary-button guest events, which the game's
normal touch controls ignored.

The widget now converts the mask to a zero-based index consistently for press,
release, and held-button movement. An empty mask maps to the driver's no-button
value, -1. The conversion remains local to Qt; shared bit utilities, guest event
semantics, and the iOS input path are unchanged.
