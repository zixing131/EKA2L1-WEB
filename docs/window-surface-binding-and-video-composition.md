# Video flicker exposes missing window-surface binding semantics

Source-guided investigation and refactor of the video replay workaround in
`7ba3ee372`, starting from `7770257d0`, on 2026-09-05.

Ferrari GT's intro flicker was traced in the earlier investigation to the game's
white EGL frames overwriting video in the same window. The patch retains the video
texture and draws it again after every EGL swap. That suppresses the observed
flicker, but gives EGL knowledge of another producer and encodes a video priority
that is not the ScreenPlay background-surface contract.

## What the original implementation says

The local graphics OSS checkout is at `ff133bc50e6158bfb08cc093b0f0055321dcde99`.
The multimedia client source was missing locally, so the corresponding
SymbianSource repository was consulted. The relevant paths are:

- [RWINDOW.CPP, surface client requests and documented contract](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/windowing/windowserver/nga/CLIENT/RWINDOW.CPP#L1683).
- [wnredraw.cpp, server binding and removal](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/windowing/windowserver/nga/SERVER/openwfc/wnredraw.cpp#L513).
- [windowelementset.cpp, replacement of the background element](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/ff133bc50e6158bfb08cc093b0f0055321dcde99/windowing/windowserver/nga/SERVER/openwfc/windowelementset.cpp#L1264).
- [mediaclientvideodisplaybody.cpp, video geometry, attachment and removal](https://github.com/SymbianSource/oss.FCL.sf.os.mmaudio/blob/a39c075cce88671b23d13598fda2f7cbf008f3bb/mmlibs/mmfw/src/Client/Video/mediaclientvideodisplaybody.cpp#L895).
- [mmfclientvideoplayerbody.cpp, legacy player and GCE/DSA selection](https://github.com/SymbianSource/oss.FCL.sf.os.mmaudio/blob/a39c075cce88671b23d13598fda2f7cbf008f3bb/mmlibs/mmfw/src/Client/Video/mmfclientvideoplayerbody.cpp#L617).
- [SimulatorGraphics guestegl.cpp, EGL creation and buffer submission](https://github.com/SymbianSource/oss.FCL.sf.adapt.SimulatorGraphics/blob/da4f21b4e695ac96915a0a885af66e66b6f9a2f7/guestrendering/guestegl/src/guestegl.cpp#L1066).

`RWindowBase::SetBackgroundSurface(config, trigger)` constructs
`TWsWinOpSetBackgroundSurfaceConfig` and sends a synchronous `WriteReply` request.
The server validates the configuration and registers/connects the surface. An
existing different background surface is replaced; configuring the same surface
updates its geometry. This is one background attachment per window, not one
background per producer. Placed scene elements are a separate facility.

The surface occupies its configured extent above the window background colour.
`CWindowGc` rendering appears in front of it. Alpha or colour-key composition
exposes the surface through the UI content, according to display mode. Drawing
an opaque default GDI clear across the surface is not an equivalent implementation.
Window-tree order still controls overlapping windows.

`CMediaClientVideoDisplayBody::SetBackgroundSurface` calculates a source viewport,
destination extent and rotation, then calls this exact window API. It intersects
the intended video extent with the clip rectangle and adjusts the source viewport
accordingly: clipping must not rescale the entire movie into the smaller rectangle.

The available SimulatorGraphics EGL implementation binds its surface during
`eglCreateWindowSurface`. `eglSwapBuffers` submits an update for that surface ID;
it does not reattach it to the window. Consequently, publishing an old EGL surface
after video has replaced the attachment does not itself reclaim the window.
This is concrete evidence for separating attachment from publication. It is not
proof of every detail of the proprietary X7 EGL implementation.

Removal also has a contract. `RemoveBackgroundSurface` queues a removal, releases
the window's registration and optionally triggers redraw; its documented lifetime
extends through the last displayed frame. The multimedia client calls `Finish()`
after detaching before proceeding with removal. `RemoveSurface` notifies the
controller after detachment, and player `Close` destroys its display objects before
closing the controller. `Stop` only stops the controller in this client code;
surface-removal events remain separate. Neither automatic restoration of a former
EGL attachment nor unconditional video detachment on Stop follows from these paths.

The original v1 player can select GCE surfaces or DSA based on controller support.
Its window and clip rectangles are display-relative, converted to window-relative
coordinates for the GCE display path. V2 supplies window-relative geometry.
Applying ScreenPlay attachment rules indiscriminately to older DSA playback would
be another compatibility error.

## What the replay workaround missed

In `dispatch/src/libraries/egl/egl.cpp`, every swap calls
`set_presented_surface`, draws it into the window command queue, and now redraws
video. In `dispatch/src/video.cpp`, every decoder callback similarly updates a
texture and appends a screen-positioned draw to that queue. The canvas stores both
`presented_surface_handle_` and `posted_video_handle_`, without a single attachment
identity or replacement operation. Producer arrival order becomes composition order.

`redraw_msg_canvas::draw` has two different compositions. Server redraw replays
EGL, video, then stored GDI. Client redraw builds pending GDI before merging the
producer command queue. Thus GDI/video order can differ between redraw paths even
after the EGL-specific replay fix. Moving that replay to the end of `draw` would
merely establish another unconditional priority, potentially covering GDI.

The guest patch loses geometry before the host sees it:

- `CVideoPlayerUtility::SetDisplayWindowL` only changes the owned window and ignores
  both supplied rectangles; construction passes the clip rectangle as the display
  rectangle and does not preserve the intended window rectangle.
- V2 `AddDisplayWindowL` ignores video extent and forwards clip as display rect.
  `SetVideoExtentL` is a stub.
- Host `set_target_rect` stores this as the destination rectangle. Configuration
  changes reach the retained frame only on a subsequent decode callback, so a
  stationary frame cannot immediately reflect them.

Clearing `posted_video_handle_` does not mark damage or schedule composition.
Removal can therefore leave old video pixels until some unrelated update. A raw
texture handle also does not keep a resource alive while queued draws still refer
to it; joining the decoder only addresses CPU callbacks, not queued GPU work.

There is an additional concrete lock inversion in the current code:

| Path | First lock | Second lock |
|---|---|---|
| `post_new_image` / unregister | `postings_lock_` | `screen_mutex` |
| `window_server_client::delete_object` / client teardown, then canvas destructor observer | `screen_mutex` | `postings_lock_` |

The two paths can wait on one another. This is a static finding, not a reproduced
hang in this investigation. More locking around the current raw window pointer is
not a sufficient lifetime design.

## Implementation

The shared window layer now owns a producer-neutral `window_surface` resource and
one background attachment. EGL binds during window-surface creation, flushes its
context at swap and copies the completed bitmap into a separately owned presented
image. Later rendering cannot mutate the displayed image, and swaps cannot reclaim
a displaced attachment. Video callbacks copy complete RGBA frames into a bounded
latest-frame mailbox. WServ uploads a frame once even when several targets share it.
The existing display scheduler polls visible streaming attachments; decoder callbacks
never acquire a screen lock or dereference a window.

ScreenPlay redraw windows retain GDI in a premultiplied RGBA bitmap and use the same
background colour → attached surface → GDI composition for client and server redraw.
Retaining pixels avoids replaying alpha operations on every video frame and preserves
non-redraw content after its command store ages out. BeginRedraw clears the replaced
UI area to transparent. Resizing carries forward existing UI pixels. Empty/default
GDI content does not introduce an opaque white clear over the surface. Blank windows
also draw the attachment. Pre-ScreenPlay video retains a separate direct-rendering
path within its window, rather than imposing ScreenPlay replacement on DSA playback.

Attachments retain stable resource ownership independently of GPU handles. Texture
replacement, draws and final destruction enter the same graphics command stream under
the kernel lock. Video registration and window observer removal also run under that
lock. Stop/join runs with the kernel lock released because completion takes it; only
after the decoder has stopped does close detach targets and release resources. This
removes the decoder/postings/screen lock inversion rather than adding another lock
around raw window pointers.

Attachment, geometry and removal operations mark window damage and schedule a
composition. Extent controls scaling; clip intersects the destination without changing
that mapping. Source crop is a separate viewport. V1 patch calls convert display-relative
rectangles using the window's absolute position, while V2 preserves window-relative
extent and clip independently. V2 SetVideoExtentL and the omitted default-window bounds
are implemented. The guest patch forwards these through new bridge entries while the
old entries remain available for previously installed patch binaries.

The configured S60 5th/GCCE UREL variant, `mediaclientvideo_v100.dll`, was rebuilt after
building its `priv.lib` dependency. The E32 validator accepts the image, with all 167
exports retained. This source has only the v100 target in `target.inf`.

## The legacy display return policy

An initial implementation used strict replacement and removal for both player APIs.
Ferrari GT's intro rendered, but video close left a white window indefinitely. A
short-lived trace showed that the game created EGL before the movie, video replaced
that same window's attachment, and close occurred without EGL recreation. Resource
loading continued normally. Treating each subsequent swap as a new attachment would
reintroduce the original ownership bug.

The v1 HLE video controller therefore explicitly borrows its target display. It keeps
a weak reference and configuration for the displaced producer, and restores that
producer only if it is still alive and the controller's own attachment is still
current. A later attachment cannot be overwritten by stale cleanup. The reference is
weak so closing EGL during playback cannot resurrect it. V2 uses strict removal;
WServ itself has neither a restoration stack nor a video priority. This is a bounded
HLE compatibility policy justified by the observed transition, not a claim about the
unavailable proprietary X7 EGL implementation. Stop retains the attachment and last
frame; close/removal performs detachment.

## Verification and limits

The macOS `ekatests` target covers replacement versus publication, conditional detach,
owned/coalesced frames, clipping and rotation, presented-buffer isolation, shared
uploads, texture retirement, publication during attachment replacement/destruction,
and premultiplied GDI pixels plus transparent redraw clearing. These tests use a
recording graphics driver; they do not substitute for a real compositor screenshot.

Release iOS Simulator verification passed the default suite (12 checks: Final Battle,
Calculator input/menu, N95 Calculator and string catalog) and Angry Birds (5 checks:
loading tap, menu, PLAY, carousel swipe and log scan). Screenshots were inspected,
including both calculators' GDI rendering and Angry Birds' OpenVG episode carousel.
The focused macOS suite passed 51 assertions in 7 cases. A 37-second capture of the
final Ferrari build contained no uniform-white guest frames in the 486 captured
frames after the first ten seconds (which exclude startup). Ferrari then reached
Quick Race and rendered the race. A separate launch exited through the app's Exit Game
menu while the intro was decoding and returned to the library without a hang. The
Ferrari logs contained no guest panic, access violation, graphics halt or temporary
surface probes. Asphalt 6 also passed all 9 regression checks, covering its intro,
menu transitions, car selection, rendered Nassau race and guest log scan; its movie
and race screenshots were inspected.

This is an internal HLE composition refactor, not a full implementation of ScreenPlay
surface IPC, colour-key modes, placed elements, or every multimedia controller feature.
Real hardware, Android rendering, legacy DSA video playback, and arbitrary third-party
video-window layouts require additional coverage. The shared macOS build and focused
tests establish compilation and resource/geometry invariants, not platform-wide visual
parity.

The earlier investigation usefully ruled out server background clears as the observed
Ferrari flicker trigger. Its broader claim that video is always an overlay above client
GL is not established by the source. The root cause addressed here is the conflation
of surface ownership, buffer publication and window composition.

## Review follow-up

Two retained-UI cases were missing from the first regression suite. The bitmap cache
updates its hash when an upload is queued in a pending GDI segment. Initializing the
UI bitmap from retained redraw commands alone therefore sees a cache hit without
uploading the pixels, and discards the pending upload. Initial UI replay now submits
pending texture updates first, without replaying their pixel draws twice.

Unmasked EColor16MAP bitmaps already contain premultiplied RGB. The retained-UI path
must copy those values directly rather than applying the source alpha again. Focused
tests cover both EColor16MA and EColor16MAP input, and upload-only replay. These tests
use the same GDI command builder that creates the retained UI.

The review follow-up passed 80 assertions in 10 focused cases after adding the
sensor-boundary tests. A fresh Release Simulator build passed the default 12 checks
and all 5 Angry Birds checks; both suites were visually inspected again.
