# Every GLES1 draw failed on macOS because `inverse()` is not in GLSL 1.40

## Symptom

Jelly Chase, built from this tree for macOS, showed nothing but a flat blue
rectangle — the clear colour — while the FPS counter sat at a plausible 14. The
same build on the iOS simulator rendered the game correctly. 7Days looked fine
at first because its intro is a 2D bitmap; only its 3D scene would have been
affected.

## Diagnosis

The emulator log had exactly one distinct error, repeated 253,425 times in a
single session:

```
Error while compiling shader: ERROR: 0:29: Invalid call of undeclared identifier 'inverse'
                              ERROR: 0:30: Use of undeclared identifier 'modelViewTrInv'
→ Fail to create shader module!
→ Fail to create GLES1 vertex shader module!
→ Problem while retrieveing GLES1 shader program!
→ Error while preparing GLES draw. This should not happen!
```

Every GLES1 draw call was being dropped, so the only thing reaching the screen
was `glClear`.

`shadergen.cpp` builds the normal matrix with
`mat3(inverse(transpose(uViewModelMat)))`, and its desktop branch emits
`#version 140`. `inverse()` did not become a GLSL built-in until 1.50. The ES
branch emits `#version 300 es`, and GLSL ES 3.00 does have it — which is the
whole reason iOS was unaffected and this never came up during the port.

The code is byte-identical to upstream and dates to `c5f86d38e` (2022). It
survived four years because most desktop GL drivers do not enforce the version
gate on built-ins; Apple's does.

Two red herrings cost time before the log was read properly:

- `Corrupted graphics command list! Emulation halt.` looks fatal but is the
  normal shutdown path — `list_queue.pop()` returns `nullopt` only after
  `abort()`, which `kill_emulator` calls. It says nothing about the failure.
- A single `Invalid bitmap handle to draw` appears in healthy sessions too.

The much better signal was collapsing the log's error lines:
`grep -a "^E " run.out | sed 's/^E [^ ]* //' | sort | uniq -c | sort -rn`. The
253,425 count made it obvious this was not an occasional glitch but the whole
pipeline.

## Fix

Emit a hand-written `mat4 eka2l1Inverse(mat4)` into the non-ES vertex shader
through the existing `external_func` slot, and call it instead of the built-in.

Raising the shader to `#version 150` would have been a smaller diff but is
wrong: `gl_context::s_desktop_opengl_versions` falls back as far as `{3, 1}`,
and `#version 150` needs GL 3.2. That trade would fix macOS by breaking any
host that lands on a 3.1 context.

The helper is emitted unconditionally on the desktop path, matching the call
site, which is also unconditional. The ES path is untouched — with `is_es` the
formatted string reproduces the original literal exactly, so GLES output is
identical byte for byte. That property is what makes the change safe to land
without an iOS regression run.

Two details worth remembering about the surrounding code:

- `uViewModelMat` is a uniform in the normal path but a *local* `mat4` computed
  from the palette matrices when skinning is enabled, so the call site is valid
  either way.
- The skinning branch is visibly broken upstream (a `{}` placeholder passed to
  `+=` rather than `fmt::format`, the closing `");\n"` inside the accumulation
  loop, and `uMatrixIndicies` misspelled against the declared
  `uMatrixIndices`). It was left alone — no title in the suite exercises it, and
  it is unrelated to this bug.

## Verification

Jelly Chase went from clear-colour-only to a complete 60 FPS render, with zero
shader errors in the log. Snakes and 7Days behave the same, on both the dyncom
and dynarmic backends. Confirmed on macOS arm64, RelWithDebInfo, Nokia 5320
(RM-409).

Upstreamed as part of EKA2L1/EKA2L1#642.
