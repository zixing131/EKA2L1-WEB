# GLES2 shader compilation on macOS Core Profile

Need for Speed: Shift exited to the application list during startup on the macOS
frontend. The guest raised KERN-EXEC 3 after several shader compilations failed;
a graphics command-list halt followed the guest failure.

The host compiler rejected `#version 120`. The GLES2 bridge chose that version for
all desktop drivers, but macOS uses an OpenGL Core Profile context. The game's
original shaders use GLSL ES 1.00 syntax, including `attribute`, `varying`, precision
qualifiers, and `gl_FragColor`. Merely changing the desktop version to 140 or 150
still rejects the legacy declarations.

A small native CGL compiler probe established that the same context accepts
`#version 100` with the original ES syntax. The driver advertises
`GL_ARB_ES2_compatibility`, whose shader-language contract explicitly requires
GLSL ES 1.00 support. This avoids an unnecessary source translator for attributes,
varyings, texture calls, and fragment outputs.

The graphics driver now exposes GLSL ES 1.00 support separately from floating-point
precision qualifier support. The GLES2 bridge uses its existing ES source path
when this capability is present. Desktop drivers without it retain the existing
GLSL 1.20 fallback, and native GLES drivers keep the ES path.

With the capability enabled, Shift passes shader compilation and reaches the
language selection screen without the guest panic or graphics halt.
