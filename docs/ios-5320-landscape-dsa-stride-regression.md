# A ScreenPlay stride fix breaks landscape DSA on the 5320

The 5320 game 天地道 continued to refresh at about 12 FPS, but most of its
landscape frame was black and the visible portion was split into repeated
horizontal bands. This appeared after the ScreenPlay framebuffer-stride fix
for Sky Force Reloaded.

The regression was not caused by a new HAL value. At the first screen update,
LLDB showed a 32-bit active DSA session and a 960-byte framebuffer pitch,
which is the unchanged `240 * 4` pitch of the 5320's physical portrait screen.
The current display mode, however, was 320 by 240 with a 270-degree rotation.
Its tightly packed landscape rows therefore required 1280 bytes.

The ScreenPlay fix selected its alternate pitch whenever any 32-bit DSA was
active. It did not also require the screen to use the ScreenPlay architecture.
That sent the older 5320 through the new branch, where the physical portrait
width was used as the row length for a rotated landscape frame. The uploader
advanced by 960 bytes instead of 1280, explaining both the repeated bands and
the missing lower portion.

As a diagnostic, changing only the live DSA count in LLDB made the upload take
the former tight-row branch. The next frame immediately displayed the
DingooGames logo and sound prompt correctly, confirming that neither the game
nor its assets had stalled.

The fix restricts the alternate aligned pitch to active 32-bit DSA sessions on
ScreenPlay screens. Every other path derives its tight pitch from the current
mode width, so rotated pre-ScreenPlay devices keep their landscape row layout.
An explicit `pixels_per_line` is supplied only when the selected ScreenPlay
pitch actually differs from that tight pitch.
