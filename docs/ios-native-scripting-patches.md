# Native-only scripting patches on iOS

## Problem

Upstream EKA2L1 ships a handful of per-game/OS compatibility patches as Lua
scripts under `src/scripts/*.lua` (AstroQuest, Eternal Legacy, Hero of Sparta,
Warhammer 40K, S60v3 Avkon, and an S^3 ROM-DLL preload). The iOS port disabled the whole
scripting subsystem (`EKA2L1_ENABLE_SCRIPTING_ABILITY OFF`), so none of those
fixes applied on iOS.

The blocker is the scripting runtime, not the engine. The Lua API layer is a thin
shell over **LuaJIT FFI**: every module calls C exports through `ffi.C.*`, and the
event hooks pass a Lua closure *as a C function pointer* (`breakpoint_hit_lua_func`).
That last step — an FFI callback — needs LuaJIT to generate a machine-code
trampoline at runtime, i.e. writable-executable memory. iOS forbids W^X on
non-jailbroken devices, so FFI callbacks cannot work there, and App Store /
TestFlight builds carry no JIT at all. Plain FFI *calls* would run in interpreter
mode, but the hook-registration API is built entirely on FFI *callbacks*.

## Key observation

The breakpoint engine itself has almost no Lua in it. Across the whole `scripting`
library, only `manager.cpp` touches LuaJIT (5 sites), and all of them are in the
"load a `.lua` file / run its entry chunk" path. Everything else — writing the
`bkpt` instruction, the kernel breakpoint-hit callback, address relocation/patch,
`cpu::set_register`, codeseg loading — is ordinary C++. Critically,
`scripts::register_breakpoint` takes a plain `void(*)()` function pointer, and the
dispatch (`call<T>()`) just invokes it. A captureless C++ lambda / free function
satisfies that with **no runtime code generation**.

So the patches don't need Lua; they need the engine plus a way to register native
callbacks. Since we don't want to expose runtime script loading on iOS anyway,
compiling the fixed patch table directly in C++ is both sufficient and simpler
than porting LuaJIT.

## What changed

A build mode split: the scripting **engine** and the LuaJIT **runtime** are now
independently selectable.

- `EKA2L1_SCRIPTING_LUA` (new CMake option, default ON) gates LuaJIT: the external
  build (`src/external/CMakeLists.txt`), the `liblua` link, the Lua asset copy, and
  a new `ENABLE_SCRIPTING_LUA` configure define.
- iOS now forces `EKA2L1_ENABLE_SCRIPTING_ABILITY ON` + `EKA2L1_SCRIPTING_LUA OFF`
  — the engine is compiled and integrated (`epoc` links `scripting`, the
  `ENABLE_SCRIPTING` breakpoint path in `epoc.cpp` is live), but no LuaJIT.
- In `manager.cpp`, the Lua-only bits (`.lua` parsing in `import_module`, the
  `lua_pcall` entry in `call_module_entry`, the folder watcher, and `lua_gc` in
  `call<T>()`) are guarded by `ENABLE_SCRIPTING_LUA`. The kernel-hook registration
  that used to sit inside `call_module_entry` was extracted into
  `register_kernel_hooks()` so it runs in both modes.
- `import_all_modules()` branches: with Lua it scans `scripts/*.lua` as before;
  without Lua it calls the new `register_builtin_patches()`.
- `builtin_patches.cpp` (new, native-only) is the C++ transcription of the six
  shipped scripts — using the same scripting manager breakpoint APIs as their
  Lua counterparts — plus the S^3 `domaincli.dll` preload guarded on
  `get_symbian_version_use() >= epocver::epoc95`. Keep it in sync with
  `src/scripts/*.lua`.

Breakpoint callbacks may change the guest PC to bypass a faulty code path. The
breakpoint handler restores the original instruction address before invoking the
callback, so a callback's final PC is preserved while ordinary callbacks still
single-step the displaced instruction before reinstalling the breakpoint.
Resolved ROM hooks retain one shared preimage instead of cycling the same physical
instruction per process. Dyncom translates Thumb `BKPT` to its internal breakpoint
form, synchronizes CPSR before dispatch, and leaves PC on the displaced instruction
when a hook stops the core for single-stepping. The scripting manager also uses
the registered address's Thumb bit during dispatch. Eager ROM registration can
resolve an absolute hook even when the system DLL was loaded before scripting and
has no current process attachment, while still requiring the requested UID/hash.

For ROM methods that vary by firmware address, `register_rom_export_breakpoint`
locates a method by Symbian export ordinal, hashes only its bytes through the next
higher export, and installs a method-relative hook only for a known fingerprint.
The Lua API exposes the same operation as
`events.registerRomExportBreakpointHook`. The S60v3 empty-menu patch uses this
path, so relocation and unrelated changes elsewhere in `eikcoctl.dll` no longer
need absolute-address or whole-DLL-hash entries.

Desktop and Android are unaffected: `EKA2L1_SCRIPTING_LUA` defaults ON, so they
still build the full LuaJIT path and load `.lua` files at runtime.

## Verification

- Debug and Release iOS simulator builds compile and link with no LuaJIT.
- Full iOS regression suite: 12/12 PASS (Final Battle, Calculator, N95 Calculator,
  string catalog) — the now-live breakpoint/kernel-hook path does not regress apps
  that have no patches.
- Boot log shows `Built-in native game patches registered`, confirming
  `register_builtin_patches()` runs at startup. The S^3 branch correctly stays
  silent on the S60v3 regression devices (rm-409/rm-320).

The RM-409 and RM-320 Avkon patches were exercised directly with 7Days: repeated
left-soft-key presses left the game on its first screen with no guest or host
crash. Calculator's populated Options menu remained a passing control.
