# N-Gage Launcher Store hangs, reports OOM, or renders black

## Symptom

On the 5320 (RM-409), moving right through the N-Gage Launcher tabs works until
the final **Store** tab. The Store header appears over an empty content area,
then the guest stops responding to every key while the iOS application itself
remains alive.

## Narrowing down

The first three tab changes continued to load bitmaps and redraw normally. The
fourth change loaded `ngibrowserengine.dll` and started `CookieServer.exe`, then
the emulator log stopped immediately after:

```
[Service.Applist]: Unimplemented applist opcode 0xA
```

The host process was idle rather than deadlocked: it used almost no CPU and
could still be inspected, but another direction-key event produced neither a
new frame nor any guest log activity. This matched a guest thread blocked in a
synchronous service request.

EKA2L1's new-architecture AppList opcode table identifies `0xA` as
`applist_request_recognize_data`. The enum and the file-handle form of the
request already existed, but the ordinary buffer form had no dispatch case.
The AppList session's default branch only logged an error and did not complete
the IPC, leaving its caller in `SendReceive` forever.

The Symbian AppArc source confirms the contract:

- `RApaLsSession::RecognizeData` sends `EAppListServRecognizeData`;
- slot 0 is a writable packaged `TDataRecognitionResult`;
- slot 1 is the file name;
- slot 2 is the data buffer;
- the server performs recognition, writes the result to slot 0, and completes
  the synchronous message.

This also rules out the nearby missing cache files and OOM-server warning as
the original freeze cause: both occur before the final AppList request, whereas
the uncompleted request exactly accounts for the permanent wait.

## Fix

The AppList server now dispatches `applist_request_recognize_data`. Its handler
wraps the slot-2 descriptor in the same read-only stream used by the existing
file-handle implementation, runs the shared MIME recognizer, writes a
zero-initialized `data_recog_result` to slot 0, and completes with `KErrNone`.
The result uses AppArc's actual signed confidence values. Every non-empty MIME
result, including the deliberate `application/octet-stream` fallback, is
`EProbable` (`100`). The fallback must remain a positive-confidence recognized
type: `EPossible` is numerically zero, which the Launcher treats as no match and
causes it to take a different online-download path. The previous literal `10`
was not a member of Symbian's discrete confidence enum. The file-handle path
now shares this result construction, so its reserved/UID data and confidence
are deterministic too.

Completing the request exposed a second compatibility error: Store displayed
`Out of internal memory (Error -4)`. A temporary request probe showed that the
Launcher was asking AppArc to recognize:

```
E:\Private\20003B78\showroom\frontpage_lg-1.xhtml
```

Its 637-byte buffer began with `<!DOCTYPE`, but EKA2L1's small built-in
recognizer returned the generic `application/octet-stream` fallback with
`EProbable` confidence. That sent the local Store front page through the wrong
content-handler path; the resulting `KErrNoMemory` was a guest error, not host
memory exhaustion.

The original S60 web recognizer classifies `.html`, `.htm`, `.shtml`, `.shtm`,
and `.xhtml` as `text/html` with `EProbable` confidence. It similarly
classifies `.xml` as `text/xml`. EKA2L1 now follows those rules, passes the
filename into the shared recognizer for both IPC variants, and advertises both
web MIME types in the supported-type array. In particular, S60 intentionally
reports XHTML as `text/html`, not `application/xhtml+xml`.

This implements the shared AppArc IPC contract rather than bypassing browser
behavior or special-casing N-Gage.

## Why the local front page was black

The Store is not waiting for a retired online service at this point.
`frontpage_lg-1.xhtml` is a fully local wrapper whose only object is:

```html
<object data="frontpage_lg-1.swf"
        type="application/x-shockwave-flash"
        height="236" width="240">
```

File tracing confirmed that the browser resolved the relative URL correctly
and opened `E:\Private\20003B78\showroom\frontpage_lg-1.swf`. The Flash Lite
DLLs loaded as well. Copying the SWF into the browser cache therefore could not
help.

Getting far enough into the plug-in exposed two missing platform contracts.
The RM-409 browser gates Flash on Feature Manager ID 1146
(`KFeatureIdFlashLiteBrowserPlugin`), and Flash opens its helper process by ID
through executive call `0x71`.

The general compatibility fixes are:

- report feature 1146 when `z:\sys\bin\npflashlite.dll` is present;
- register executive call `0x71` as `process_open_by_id` on Symbian 9.3.

After those changes, forcing a redraw no longer produced a uniformly black
window, but the Flash image appeared as a diagonal trail of colored pixels.
That visual signature isolated the final defect to the iOS bitmap upload.
Flash draws a 246-pixel-wide, 24-bpp bitmap. Symbian/OpenGL pads each row to
four bytes: `246 * 3` is 738 bytes, so the actual stride is 740. The iOS GLES
path manually expands BGR to RGBA and advanced by an integral
`pixels_per_line`; integer division reduced 740 bytes to 246 pixels, and the
converter consequently started every row two bytes early. It now computes the
source byte stride using the same four-byte unpack alignment as the normal GL
upload path. This is a general fix for every padded 24-bpp FBS bitmap, not a
Flash-specific exception.

That fixed the diagnostic redraw, but the normal Store path was still black.
Running the extracted 102,170-byte `frontpage_lg-1.swf` with Ruffle CLI proved
that the asset itself is complete and renders the expected Store page. The
remaining failure was in two host contracts exercised while the browser handed
the local SWF to Flash.

First, Window Server repeatedly returned a zero-area invalid rectangle.
`region::clip()` had assigned the bounding X coordinate when clipping the top
edge, and retained rectangles whose intersection had zero width or height.
When the plug-in resized its child window through zero, that stale rectangle
kept the window on the redraw queue forever. Region clipping now uses geometric
intersection, removes empty rectangles, and removes a resized window from the
redraw queue when no invalid area remains. This follows WSERV's
`ClipInvalidRegion`/`RemoveFromRedrawQueueIfEmpty` behavior.

Second, the browser copied the SWF through `RFile::Temp`. EKA2L1 returned the
VFS spelling `c:/system/temp/...` instead of a Symbian path, and file-sharing
bookkeeping treated slash variants as different keys across open, duplicate,
rename, and close. Temporary names and sharing keys are now canonicalized to
Symbian separators, and a subsession rename moves its sharing state to the new
name.

The decisive defect was the file-server initialization mask:

```cpp
FLAG_INITED = 0
```

Because testing a zero mask can never succeed, every new `RFs` connection ran
`fs_server::init()` again. Initialization deletes stale files from every
`?:\System\Temp\` directory. Flash creates a new file-server session just after
the browser has written and closed its renamed temporary SWF, so that repeated
initialization deleted the live SWF before Flash could open it. Flash reported
`KErrNotFound`; the browser's error callback synchronously cancelled and
destroyed its own local-file active object, whose `RunL()` then continued into
`SetActive()` and panicked with `E32USER-CBase 49`. The apparent active-object
lifetime bug was therefore secondary.

`FLAG_INITED` is now bit zero (`1 << 0`), so temp-directory cleanup runs once
when the file server starts, as intended. This is a general file-server fix,
not an N-Gage exception.

## Verification

With the Release simulator build on RM-409, selecting **Get More Games** reaches
the Store without an unimplemented AppList opcode, frozen guest, out-of-memory
dialog, or `CBase 49` panic. The local SWF renders **Game of the Week**,
**Available games**, and **Latest Games**, including the Brain Challenge and
Dirk Dagger artwork, with correct rows and colors.

The Release iOS regression suite also passes 12/12 both with a fresh install
and on the immediately repeated run.
