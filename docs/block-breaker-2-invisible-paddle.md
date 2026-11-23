# Block Breaker 2 Deluxe has no paddle

## Symptom

On the X7 (rm-707), Block Breaker Deluxe 2 (`0x20024078`) boots, plays its intro,
reaches the island map and starts level 1 — but the paddle at the bottom of the
playfield is never drawn. The ball sits in mid-air where the paddle should be, the
"TOUCH THE SCREEN TO PLAY" prompt appears, and the level is unplayable because you
cannot see what you are steering.

Everything else in the game renders correctly: HUD, bricks, background, the animated
silhouette behind the playfield. Only the paddle is missing.

## Narrowing it down

The game runs on the GLES1 HLE (`enable-hw-gles1`), and the emulator log had nothing
useful in it — no unimplemented opcode, no `Vertex attribute not bound to a valid
buffer`, no GL error. So the next step was to dump, for one frame, the state of every
`egl_context_es1::prepare_for_draw` call: vertex count, client-array bitfield, the
enabled non-shader state, the bound texture, and the top of the modelview and
projection stacks.

That immediately separated the ~19 draws of a gameplay frame into 17 sprite quads
(`size = 2` positions) and two real meshes (`size = 3`): one of 1836 vertices — the
dancer silhouette — and one of 2508 vertices. The 2508-vertex one is the paddle, and
its modelview matrix printed as **all NaN** while every other draw in the same frame
had a sane matrix.

A second probe that flagged every matrix operation which turned a finite matrix into a
NaN one named the culprit on the first run, 439 times:

```
GLESPROBE-NAN rotatef ang=0 axis=(0,0,0)
```

### Root cause 1 — `glRotatef` with a zero-length axis

The game emits `glRotatef(0, 0, 0, 0)` while building the paddle transform. EKA2L1
forwards that straight to `glm::rotate`, which normalises the axis: `(0,0,0)` becomes
`0/0`, and the NaNs spread through the whole modelview matrix, so every paddle vertex
lands outside the clip volume and nothing is rasterised.

Treating the zero axis as a no-op prevents the matrix corruption. Nonzero axes must
still be normalised, as specified by the GLES 1.1 `glRotate` contract. In particular,
`glRotatex(90 * 65536, 0, 0, 1)` rotates 90 degrees around Z even though its axis
length is only 1/65536.

The local Symbian OSS `opengles/openglesinterface` contains Khronos-compatible
headers, import-library definitions and empty export stubs, not a renderer. The
Belle SDK declarations and SymbianSource search do not establish how a particular
phone driver handles a zero axis. Mesa provides a compatibility precedent, but its
`1e-4` cutoff only applies after its single-coordinate-axis fast paths. Applying
that cutoff unconditionally would discard valid small-axis rotations that both
GLM and Mesa preserve. There is no evidence that Symbian drivers derive from Mesa.

Disassembling and executing the 5320 ROM's actual ARM rotation code confirmed that
its zero-angle/zero-axis call produces identity, while tiny nonzero axes rotate
normally. The X7 ARM DLLs forward the arguments to VideoCore without filtering them.
See [the ROM investigation](./gles1-rotation-axis-contract.md) for addresses, binary
identities, instruction-level results and the limits of the zero-axis fallback.

References: [GLES 1.1 glRotate](https://github.com/KhronosGroup/OpenGL-Refpages/blob/main/es1.1/glRotate.xml),
[Symbian import library](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/master/opengles/openglesinterface/group/gles11_implib.mmp),
[Mesa rotation](https://github.com/intel/external-mesa/blob/master/src/mesa/math/m_matrix.c#L741-L807).

### Root cause 2 — an upload silently dropped, so the paddle was untextured

Skipping degenerate rotations made the paddle appear — as a garbled cloud of noise
with roughly the right silhouette. The per-draw dump showed why: its texture (name 6)
had `internal_format = 0` and size `0x0`. Dumping the whole object table showed the
same for names 1 through 9, while everything from 10 up was fine.

Logging the texture lifecycle explained the split. The game's loader does this for its
first few textures:

```
glGenTextures(4)          -> 6 7 8 9
glDeleteTextures(1, {6})
glBindTexture(GL_TEXTURE_2D, 6)
glTexImage2D(..., 128x128, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, data)
```

Binding a deleted name is legal: in GL it creates a new texture object with that name,
bound to that target. `egl_context_es1::bind_texture` did not create it — it only
recorded the name, and skipped `try_bind` because the slot was empty. The object was
then created lazily by `binded_texture()` inside `glTexImage2D`, which is too late:
that path has no idea what target the game bound, so the object stayed
`GLES_DRIVER_TEXTURE_TYPE_NONE`. Three lines further down, `tex->target_matched(target)`
rejects a typeless texture with `GL_INVALID_OPERATION` and `glTexImage2D` returns
before uploading anything or assigning a driver handle. The texture never got any
pixels, and later draws sampled whatever was left in the unit.

The ES2 context already got this right — `egl_context_es2::bind_texture` revives an
empty slot and then calls `try_bind`. Only the ES1 path was missing it.

Worth noting the two bugs hid each other: as long as the matrix was NaN nothing was
rasterised, so the missing upload was invisible.

## Fix

`src/emu/dispatch/src/libraries/gles1/gles1.cpp`:

- `gl_rotatef_emu` / `gl_rotatex_emu` return without touching the matrix stack when the
  axis is exactly zero. Small nonzero axes retain their rotation.
- `egl_context_es1::bind_texture` creates the texture object when the named slot is
  empty, so `try_bind` runs at bind time and the object learns its target. The
  subsequent `try_bind` is also now guarded by an object-type check, since ES1 keeps
  textures and buffers in the same identity container.

The paddle renders as the intended chrome/glass bar, moves with touch, and the ball
rests on it. Full simulator regression suite plus the Angry Birds touch suite stay
green, and Asphalt 6 (the other GLES1-heavy X7 title) still renders its menu car
correctly.

## Dead ends worth skipping

- The bound texture being an incomplete `128x128` upload looked at first like a
  compressed/paletted-format problem. It was not: the format path is fine, the upload
  simply never ran.
- `eglCreateContext` with a share list is rejected by EKA2L1 with a loud error, and the
  log had none, so "textures uploaded in another context" was ruled out early; logging
  the context pointer in both the upload and the draw confirmed a single context.
- The `Texture name N was previously deleted, generate a new one` warning that fires
  four times at startup is a *symptom* of the bind-time gap, not a harmless quirk. It
  is worth treating as a red flag rather than noise.
