# GLES1 rotation axes: Symbian ROM evidence

Block Breaker Deluxe 2 builds its paddle transform with `glRotatef(0, 0, 0, 0)`.
Passing this to GLM normalises the zero axis into NaNs. A proposed `1e-4` axis-length
cutoff prevented that corruption but also discarded valid nonzero axes, including
`glRotatex(90 * 65536, 0, 0, 1)`.

The GLES 1.1 specification, section 2.10.2, defines the rotation using
`u = v / ||v||`. Axis magnitude therefore does not change a nonzero axis's rotation.
The formula does not define a result for the zero vector. Mesa is a useful
compatibility precedent, but its `1e-4` cutoff only runs after single-coordinate-axis
fast paths; it is not a universal cutoff even in Mesa. There is no basis for saying
that every Symbian renderer descends from Mesa.

## What the SDK can establish

The Symbian OSS `opengles/openglesinterface` directory contains headers, import
libraries and empty export stubs. Its ABI fixes `glRotatef`/`glRotatex` at ordinals
109/110 in `libGLES_CM`, and 128/129 in `libGLESv1_CM`.

The Belle SDK's WINSCW `libGLES_CM.dll` is a real dispatcher rather than those empty
source stubs: the rotation exports load an index and jump through a function table.
The wrapper itself does not establish the renderer's zero-axis behaviour.

## Executing the 5320 renderer's ARM instructions

The rm-409 ROM's `sys/bin/libgles_cm.dll` contains the renderer itself. The image's
SHA-256 is `d3e7ca67e6fd96320fde5bdf9d998085d3f16828b42207647afeba2d66420306`.
Its code is mapped at `0x8062e1c8` and the ordinal table is at `0x8065ac30`.

- Ordinal 109 resolves to `glRotatef` at `0x80636720`.
- Ordinal 110 resolves to `glRotatex` at `0x80636888`. It converts all four 16.16
  arguments with `0x8063335c`, then branches into `glRotatef`.
- `0x80656160` aligns the three axis mantissas to a common exponent.
- The normaliser at `0x80657f08` sums their squares. At `0x80657f40` it tests the
  entire 64-bit sum for zero; a zero sum jumps to `0x80657fec`, which stores the
  original zero components. Nonzero sums follow the normalisation path without an
  absolute axis-length epsilon.

To check the interpretation, the unmodified ARM instructions were executed in
Unicorn. Only the current-context lookup (`0x8063180c`) and active-matrix lookup
(`0x80633ad0`) were replaced with synthetic pointers. Execution stopped at
`0x8065651c`, immediately after constructing the rotation's three columns and before
multiplying them into the current matrix. The renderer's fixed conversion,
normalisation, trigonometry and matrix construction all executed from the ROM.

| Inputs | Result from the ROM rotation block |
|---|---|
| float/fixed angle 0, axis `(0,0,0)` | Identity |
| float angle 90, axis `(0,0,1)` | Z quarter-turn |
| float angle 90, axis `(0,0,1/65536)` | Same Z quarter-turn |
| fixed angle `90*65536`, axis raw `(0,0,1)` | Same Z quarter-turn |
| float angle 90, axes `(1,1,0)` and `(1/65536,1/65536,0)` | Identical rotations |
| fixed angle `90*65536`, axis raw `(1,1,0)` | Same diagonal-axis rotation |
| float angle 90, axis `(0,0,0)` | Approximately `cos(90°) * I`, not identity |

The last case matters: this renderer keeps a zero vector finite, but does not treat
every zero-axis call as a no-op. The game's zero-angle call does yield identity.
These are instruction-level results, not a claim of testing on physical hardware.

## Following the X7 wrappers

The rm-707 `libGLESv1_CM.dll` has SHA-256
`14eea4e2980766ad61d23f067ef040799633ebfbccb8a8fd10734653f6eb0974`.
Its rotation exports at link addresses `0x8670`/`0x8678` jump through table offsets
`0x1f8`/`0x1fc`. The loader fills those slots with ordinals 202/203 from `khclient_vc3`.

That DLL's SHA-256 is
`a59fb49300eb332b45a9c4c30d5a616b91dc2d3dd780e2d20ab0afeb31d4efaf`.
The corresponding functions are at `0x19d48`/`0x19dc4`. They check for a GLES1 context
and send the four argument words with command IDs `0x1002`/`0x1024` through
`0x21784`, which copies the command into the transport buffer. The apparent float
helper at `0x1be90` only moves bits through an FP register; it does not normalise or
convert them to fixed point. There is no axis-length filter in these ARM entrypoints.

The matrix operation is handled beyond this client, on the VideoCore side. This
trace does not prove the X7 GPU's final zero-axis matrix. Treating the ARM wrapper
as the renderer, or inventing GPU behaviour from its exports, would be a dead end.

## Compatibility choice and verification

Ignore exactly zero axes in EKA2L1's float and fixed entrypoints, keeping the current
matrix untouched. This fixes the observed zero-angle corruption and follows Mesa's
zero-axis no-op precedent. It is a chosen finite fallback where the GLES formula
cannot normalise the axis, not a claim that every Symbian driver behaves identically
for nonzero angles and zero axes. Keep all nonzero-axis handling on the existing GLM
path; do not introduce an arbitrary absolute threshold.

A temporary C++ harness compiled the actual two bridge bodies with the repository's
GLM and fixed-point conversion, substituting only context lookup. Mathematical
right-hand-rule fixtures covered signed zero, unchanged nonidentity matrices,
positive/negative coordinate axes, small diagonal axes, float/fixed entrypoints and
missing contexts. With ASan/UBSan, the corrected implementation passed all 38 checks;
the blanket-cutoff version failed 20. No ROM bytes or reverse-engineering artifacts
belong in the upstream change.

The Release simulator build passed the default regression suite (12 checks), Angry
Birds touch suite (5 checks), and Asphalt 6 through an actual Nassau race (9 checks).
Block Breaker Deluxe 2 level 1 was also checked visually: the chrome paddle renders,
holds the ball and follows touch input. The final readability-only change moves the
fixed-point zero check before conversion; all 38 bridge checks still pass.

References: [GLES 1.1 specification](https://registry.khronos.org/OpenGL/specs/es/1.1/es_full_spec_1.1.pdf),
[Symbian GLES ABI](https://github.com/SymbianSource/oss.FCL.sf.os.graphics/blob/master/opengles/openglesinterface/eabi/opengles11u.def),
[Mesa rotation implementation](https://github.com/intel/external-mesa/blob/master/src/mesa/math/m_matrix.c#L741-L807).
