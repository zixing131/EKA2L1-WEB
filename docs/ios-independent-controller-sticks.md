# Independent controller stick bindings on iOS

A connected Pro Controller's right stick could not trigger guest keys or be
captured in the mapping editor. The iOS host-input enumeration covered R3, but
omitted the right stick's directions. Both capture and runtime input iterate
that enumeration, so neither path observed those axes.

The left stick had a separate limitation: capture translated it to d-pad
tokens, while runtime explicitly looked up the d-pad's mappings. Changing one
therefore changed the other. This was an iOS mapping issue; no controller-model
special case or shared emulator change was needed.

Both sticks now have four independent host tokens, using the same 0.45 axis
threshold for capture and runtime. Direction labels identify the stick rather
than relying on a controller's potentially ambiguous direction-button name.
The default left stick and d-pad both drive guest directions; the right stick
defaults to 2/8/4/6 for up/down/left/right. Rebinding moves only the captured
host input, while replacing any existing bindings on its target guest key.

Unversioned saved mappings inherit left-stick bindings from their d-pad entries
to preserve the user's layout. Saving marks the layout as independent, so an
intentionally cleared left-stick binding does not reappear on reload. Existing
custom layouts receive no unsolicited right-stick bindings.

GameController snapshot checks cover cardinal and diagonal input, the dead
zone, neutral release, independent stick/d-pad input, R3, migration, rebinding,
and saved unbinding. On the installed Release build, the user confirmed that
the physical Pro Controller's two sticks and d-pad could be bound separately
and produced input in the game on iPhone Air.
The Release simulator regression suite passed all 12 checks, with five more
passing checks in the Angry Birds touch suite; screenshots confirmed guest
rendering and the logs contained no reported guest crashes.
