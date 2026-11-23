# N-Gage folder import deadlocks while the guest is idle

## Symptom

On a booted N-Gage device, choosing **Install N-Gage Game**, selecting the
Ashen card folder, and confirming the Files picker left the home screen at
“Installing N-Gage game…” indefinitely. The selected folder was valid: it
contained `System/Apps/6R21/6R21.aif`, and iOS had granted the expected
security-scoped access.

## Narrowing it down

The Files picker callback did run, because the busy banner appeared. A sample
of the live simulator process showed the import worker stopped before any
directory scan or copy:

```
ContentView.handleNGageImport
  EKA2L1Bridge.installNGageGame
    -[EKA2L1Emulator installNGageGameAtFolderPath:]
      std::mutex::lock
        __psynch_mutexwait
```

It was waiting for `emulator::loop_mutex`. At the same time the Symbian OS
thread owned that mutex inside `symsys->loop()` and was asleep in
`thread_scheduler::switch_context()` → `common::event::wait()`.

This is an intentional CPU-saving behavior: when no guest thread is runnable,
the scheduler parks the host OS thread. The loop mutex remains held for the
whole tick. Consequently, an unrelated host thread cannot safely acquire the
mutex by merely waiting; nothing necessarily wakes the guest scheduler so the
tick can end.

The directory provider and security-scoped URL were red herrings. A permission
failure would have returned an installer error, while the sample proved the
installer had not reached filesystem code.

After fixing the deadlock, the same import returned immediately but claimed
the folder had no `system` directory. LLDB showed the exact Files URL passed to
the core, and `NSFileManager` could enumerate `System`, proving the scope was
valid. This exposed two older bugs in the shared case-sensitive fallback:

- `find_case_sensitive_file_name()` compared the requested entry type, but did
  not enable detailed directory iteration. On POSIX, `dir_entry::type` is only
  populated in detail mode, so an uppercase `System` directory could never
  match the requested `FILE_DIRECTORY`.
- After successfully resolving an uppercase `Apps` directory, the N-Gage
  installer returned `ngage_game_card_no_game_data_folder` unconditionally.
- Lowercase-destination copying reused the transformed destination-relative
  path to walk the source. After seeing `System`, it therefore tried to open
  lowercase `system` on the case-sensitive Files storage, stopped recursion,
  and copied nothing. The N-Gage installer ignored `copy_folder()`'s result
  and nevertheless reported success.

## Fixes

The N-Gage bridge now follows the existing exclusive-system mutation protocol:

1. take `session_mutex` so the `symsys` object cannot be rebuilt or destroyed;
2. set `mounted` false so no new loop tick starts;
3. call `stop_cores_idling()` to wake an idle tick;
4. acquire `loop_mutex` after that tick returns;
5. install the card, restore the previous mounted state, and release the locks.

The shared `pause_loop_and_lock()` helper already implements steps 2–4 for
device install, boot, deletion, and other system mutations. Reusing it fixes
the idle deadlock without changing emulator scheduling or the cross-platform
N-Gage installer.

The shared directory-name lookup now requests detailed entries before applying
its type filter, with a regression test using a mixed-case directory. The
installer's stray unconditional failure after resolving `Apps` was removed.
Folder copying now tracks source and destination relative paths independently,
and rejects an invalid source iterator instead of silently treating it as an
empty directory. The N-Gage installer propagates a copy failure rather than
showing a false success. The regression test recursively copies a mixed-case
source tree into a lowercased destination. Together these changes let standard
card images with `System/Apps` casing pass the same general installer used by
the other frontends.
