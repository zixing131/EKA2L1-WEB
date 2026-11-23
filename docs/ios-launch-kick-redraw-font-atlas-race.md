# Launch screen-kick vs. animation redraw — FreeType font-atlas race

## Symptom

TestFlight build 260771 crashed with `EXC_BAD_ACCESS (SIGSEGV)` a couple of
seconds after launching an app. The crashing thread faulted inside

```
eka2l1::epoc::adapter::freetype_font_adapter::get_glyph_atlas
  <- eka2l1::epoc::font_atlas::draw_text
  <- gdi_command_builder::build_command_draw_text
  <- redraw_msg_canvas::draw
  <- screen::redraw(...)
  <- animation_scheduler::invoke_due_animation   (ntimer thread)
```

The `.crash` report showed a *second* thread simultaneously deep inside the same
font path — `FT_Load_Glyph -> TT_Hint_Glyph -> TT_RunIns` — this one dispatched
from `eka2l1::ios::kick_screen_redraw` (`IosEmulator.mm`) on a
`QOS_CLASS_USER_INITIATED` global queue. Two threads rasterising glyphs through
the same FreeType face at once is the tell: the fault address was null, a
classic shared-cache-corruption signature rather than a bad guest pointer.

## Root cause

`screen::redraw(graphics_driver*)` walks the guest window tree and rasterises
text through the process-wide FreeType font atlas, which is **not** reentrant.
It has exactly two callers:

1. `animation_scheduler::invoke_due_animation` (window server, ntimer thread) —
   already serialises the redraw under `kern_->lock()` **and**
   `scr->screen_mutex` (see `scheduler.cpp`).
2. `eka2l1::ios::kick_screen_redraw` — an iOS-only launch helper. After a launch
   it is `dispatch_after`-ed onto a global queue at +0.5s and +1.5s to flush a
   stale/black first frame, and it called `scr->redraw()` **holding neither
   lock**.

So on any launch where the guest was already animating (the home-screen /
loading-screen redraw fires on the ntimer thread), the timed kick raced the
animation redraw straight through FreeType, corrupting the glyph cache and
eventually dereferencing a null cache slot. The 0.5s/1.5s delay is exactly the
window where a freshly launched app is drawing its first animated frames, which
is why it reproduced right after launch.

## Fix

Make `kick_screen_redraw` take the identical locks in the identical order the
animation scheduler uses (`kern->lock()` then `scr->screen_mutex`) around the
`set_screen_mode` / `redraw` pair. This serialises the launch kick against the
window-server redraw so only one thread is ever inside the font atlas. The
lock order matches the established `kern > screen_mutex > present_mutex`
hierarchy (the redraw's fired present callback takes `present_mutex` underneath),
so no new inversion is introduced. `submit_screen_frame` afterwards is unchanged
— it only re-presents the finished texture under its own `present_mutex`.

## Notes / dead ends avoided

- This is a host lifetime/threading race, not a bad guest request — the guest
  window tree and font data were valid; the corruption came purely from
  concurrent host rasterisation.
- The other iOS present path, `re_present_screen` / `submit_screen_frame`, is
  already safe: it never calls `redraw`, only re-presents the existing texture,
  so it needs no guest-state lock.
