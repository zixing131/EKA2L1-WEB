# Restoring the `ekatests` build

## What had broken

The macOS `ekatests` target no longer built after the iOS port had been built
in the same checkout. Fixing the first compiler error exposed a missing
scripting header, then missing frontend symbols at link time. Once the binary
ran, eight assertions failed across bitmap allocation, numeric conversion, and
directory handling.

These were independent problems that had accumulated because the test target
had not recently been built and run end to end.

## Stale applist call

The applist registration parser gained an `app_path_oldarch` argument in 2023,
but its test still used the old three-argument call. The fixture expects
`C:\System\Programs\...`, so passing `true` preserves the exact behavior the
test is meant to cover rather than weakening its assertion.

## Build trees shared generated configuration

`common/configure.h` and `common/version.h` were generated into the source
tree. An iOS configuration with native scripting enabled could therefore
overwrite the headers later consumed by a macOS test build configured with
scripting disabled. CMake correctly omitted the scripting target, while
`epoc.cpp` saw the iOS header and tried to include `scripting/manager.h`.

Changing preprocessor guards or enabling scripting in the test build would
only hide this cross-build contamination. The generated headers now live under
each target's binary include directory, which is exported ahead of the source
include directory. Simulator, device, desktop, and test build trees can no
longer overwrite each other's feature configuration.

## Headless frontend contract

`ekatests` links services and dispatch code that reference browser launching
and host input dialogs. Production frontends supply those functions through
Qt, Android, or iOS, but the headless test executable supplied none of them.
The test target now owns small stubs: unavailable UI operations fail cleanly,
and an accidentally requested yes/no dialog completes as cancelled instead of
leaving an asynchronous request pending.

## Runtime failures

The remaining assertions identified three common-layer issues:

- `bitmap_allocator::allocated_count()` counted zero bits as allocated, as the
  allocator requires, but its old tests still constructed words as if one bits
  were allocated. The implementation also shifted by 32 for a full word and
  skipped single-bit ranges. The range mask is now defined for every width,
  both endpoints are inclusive, and tests allocate cells through the public
  allocator operation.
- `basic_pystr::as_int()` left the automatically detected base at `-1` for
  ordinary decimal strings whose first character was not zero. Decimal is now
  the auto-detection default, prefixes override it only in auto mode, and empty
  or invalid-base inputs return the requested default safely.
- A standard directory iterator interpreted a directory path without a
  trailing separator as a filename filter. Calls with an empty filter now
  preserve directory intent, restoring case-sensitive-name lookup, recursive
  copy traversal, and deletion.

With these fixes, the scripting-disabled macOS target links and all `ekatests`
cases pass.
