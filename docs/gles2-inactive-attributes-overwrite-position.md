# Inactive GLES2 attributes overwrote the wheels' vertex positions

Need for Speed: Shift (`0x20036F92`) on the X7 reached its animated main menu
with a correctly rendered car body, but empty wheel arches. The background and
menu controls continued to render normally.

The guest loaded both GLES libraries, but a draw-path trace established that the
menu used GLES2 programs. Investigating GLES1 matrix palettes or fixed-function
lighting would therefore have missed the failing path. The wheel meshes reached
the indexed draw path, their shader linked, and the host draw produced no GL
error. Missing textures and failed shader compilation were not the explanation.

The decisive evidence was the linked attribute locations. The guest explicitly
bound position to slot 0, normals to slot 1, and texture coordinates to slot 5.
The simple textured program did not consume normals. Apple's linker assigned
its texture coordinates to host slot 0 and its position to host slot 1. EKA2L1
emulates explicit guest bindings by routing vertex descriptors to the host's
linked locations, rather than forcing those locations before the host link.

The descriptor builder iterated every enabled guest attribute. It first routed
guest position 0 to host slot 1, then treated the unused guest normal 1 as an
identity mapping and wrote another descriptor to host slot 1. The later
`glVertexAttribPointer` replaced position data with signed-byte normals. The
wheel geometry collapsed instead of appearing inside the wheel arches. Scenery
using the same simple shader with its normal array disabled did not overwrite
position. The car body's paint shader actively consumed normals and had a
different, non-conflicting mapping, which explains the selective symptom.

This is a shared GLES2 translation bug exposed by the host linker's location
assignment, not a ROM, game asset, or iOS resource-loading problem. GLES permits
enabled arrays that the current executable does not consume; they must not
overwrite an active input during translation. The relevant contracts are the
active-attribute and attribute-binding rules in the
[OpenGL ES 2.0 specification](https://registry.khronos.org/OpenGL/specs/es/2.0/es_full_spec_2.0.pdf).

The descriptor builder now enumerates the current program's active attributes
and reverse-maps each host location to its guest source. It retains consecutive
locations for matrix columns and the existing path for constant attributes.
Switching programs also invalidates the descriptor selection, even if the
guest's array pointers and first vertex have not changed. No game-specific
condition or shader replacement is involved.

A Release simulator build restored the front and rear wheels in the rotating
X7 menu scene. The default regression suite passed all 12 checks, including
Final Battle and both Calculator variants; the Angry Birds suite passed all
five loading, touch, and carousel checks. Asphalt 6 passed all nine checks,
including a rendered Nassau race. Their resulting screens were also reviewed
visually. The user also confirmed that the front and rear wheels render correctly
on the iPhone Air with the signed Release build.
