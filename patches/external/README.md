# External submodule patches (WASM build)

These patches fix build/runtime issues that only affect the Emscripten/WASM
target. They live here because the affected submodules point at their upstream
remotes, so the changes cannot be committed back through the submodule pointer.

After a fresh `git submodule update --init`, re-apply them before building the
WASM target.

| Patch | Submodule | What it does |
|-------|-----------|--------------|
| `capstone-wasm.patch` | `src/external/capstone` | Bump `cmake_minimum_required` to 3.5 and set policy `CMP0048` to `NEW` so it configures under the Emscripten CMake toolchain. |
| `fmt-wasm.patch` | `src/external/fmt` | Wrap `FMT_USE_CONSTEVAL` in `#ifndef` so the build can force `-DFMT_USE_CONSTEVAL=0` (consteval path is broken under this toolchain). |
| `yaml-cpp-wasm.patch` | `src/external/yaml-cpp` | Replace `std::stringstream` integral/float serialization with `snprintf`. `std::stringstream` constructs a `std::locale`, which crashes in Emscripten/WASM. |

## Apply

From the repository root:

```bash
git -C src/external/capstone apply "$(pwd)/patches/external/capstone-wasm.patch"
git -C src/external/fmt      apply "$(pwd)/patches/external/fmt-wasm.patch"
git -C src/external/yaml-cpp apply "$(pwd)/patches/external/yaml-cpp-wasm.patch"
```

Use `git -C <submodule> apply --check <patch>` first to verify a patch still
applies cleanly after a submodule update.

## Regenerate

If you change a submodule again and want to refresh its patch:

```bash
git -C src/external/fmt diff -- include/fmt/base.h > patches/external/fmt-wasm.patch
git -C src/external/capstone diff                  > patches/external/capstone-wasm.patch
git -C src/external/yaml-cpp diff                  > patches/external/yaml-cpp-wasm.patch
```
