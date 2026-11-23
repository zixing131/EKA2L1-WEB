# System-language switch didn't refresh the app-list captions

## Symptom

Changing the guest **System language** in iOS Settings updated the kernel locale,
but the home app list kept showing the app names in the *previous* language until
the whole emulator was relaunched. The Android/Qt frontends don't hit this because
they rebuild their app list differently after a language change.

## Root cause

Two independent reasons the captions stuck:

1. **`applist_server` caches each registration's caption in the language it was
   first loaded in.** `rescan_registries()` re-reads registry files, but
   `load_registry()` / `load_registry_oldarch()` short-circuit when the `.rsc`
   file's modification time is unchanged (`last_rsc_modified == last_mof` →
   `return false`). A language switch doesn't touch the registry files, so the
   freshness check skips the reload and the old-language `long_caption` survives.

2. **The frontend never asked for a rescan.** `setSystemLanguageCode:` flipped the
   kernel language + locale property, but nothing told `ContentView` to re-read the
   app list, so even a correct reload wouldn't have surfaced.

## Fix

- In `IosEmulator.mm setSystemLanguageCode:`, after updating the kernel language and
  locale property, drop the cached registrations
  (`applist_server::get_registerations().clear()`). The next `rescanApps()` then
  reloads every caption from scratch under the new current language, bypassing the
  mtime freshness skip.
- `SettingsView` posts `eka2l1AppListInvalidated` after the language change;
  `ContentView` observes it and re-runs `rescanApps()`, so the names update live
  without a relaunch.

Verified on the 6680 (rm-36) ROM, which ships English/Spanish/French captions:
switching English↔Spanish in Settings now re-labels the whole app grid immediately.
