# One unanswered IPC message black-screened every app on a device

## Symptom

On an iPhone, every app on the N95 (rm-320) opened to a black screen and stayed there. Every
other ROM on the same install was fine. The device had worked for days before this.

Three things the user tried, none of which helped, and all of which turn out to be diagnostic:

- installing an older EKA2L1 build,
- deleting the N95 in Settings and installing the ROM again,
- both together.

## Narrowing down

The emulator log for a launch ends like this, with no panic, no access violation, and nothing
after it:

```
Service.UI: Status pane redrawed
Service.Loader: Loaded library: 101f84b9.dll
Service.UI: Unimplemented IPC opcode for AknIconServer session: 0x3
```

Launching Calculator on a 5320 (rm-409) from the same install and the same build carries on
past that point — `Calcsoft.ini`, `avkonfep.dll`, `Unblanking screen` — and draws. So the guest
was not crashing on the N95; it was stopping. Two lines in the diff between the two logs matter:

- `Loaded library: 101f84b9.dll` appears only on the N95, and 0x101F84B9 is the S60 default
  theme package;
- the skin the two devices report differs — `Active skin UID: 0x101FD60B` on the N95 against
  `0x200039F5` on the 5320.

### What survives a reinstall, and why that is the shape of the bug

`drives/z/<firmcode>` is per device. `drives/c` and `drives/e` are **shared by every installed
device**, and the servers that keep per-device state there name a folder after the firmware
code — the central repository writes `C:\private\10202be9\persists\<firmcode>\<uid>.cre`
(`central_repo::write_changes`), the message store `C:\private\1000484b\mail2\<firmcode>\`.

Deleting a device only dropped drive Z and the ROM image, so none of that went away, and an
older build read exactly the same files. Anything wrong in there is invisible to every repair
the UI offers. That matches the report precisely.

### The value that was wrong

The `persists/rm-320` folder was recovered before the data was lost and decoded against the CRE
layout in `cre.cpp`. Comparing it against the folder a healthy install regenerates afterwards:

| repository | broken | healthy |
| --- | --- | --- |
| `10272618.cre` | byte-identical | byte-identical |
| `101f876e.cre` | same entries | same entries |
| `101f876d.cre` | key `0x7` = 1 | key `0x7` = 0 |
| `101f876f.cre` | keys `0x2`/`0x13` = **0x101FD60B** | keys `0x2`/`0x13` = **0x2000A62A** |

`101f876f` is owned by `0x10207114`, AknSkinSrv; those two keys are the active and default
skin. `0x2000A62A` is what the N95 ROM ships (`101f876f.txt` has `0x2 string "536913450"`) and
it exists on disk as `Z:\private\10207114\import\2000a62a\nseries07_01.skn`. `0x101FD60B`
existed nowhere: a listing of all 122632 files in the app container had no hit for it, and the
N95 ROM carries only `101f84b9` and `2000a62a/b/d/e`. The 5320's `0x200039F5`, by contrast, has
its `import/200039f5/` folder — so the value being resolvable is the norm, and this one was not.

`akn_skin_server` handles that: `find_skin_file` misses, and it falls back to
`DEFAULT_ALWAYS_EXIST_SKIN_PID` = 0x101F84B9, the S60 default theme. That fallback is what put
the guest on the `101f84b9.dll` path, and that path asks AknIconServer for opcode 3.

### Why an unimplemented opcode is fatal rather than cosmetic

`akn_icon_server_session::fetch` logged the unknown opcode and `break`ed without completing the
message, and `typical_server::process_accepted_msg` does not complete on the session's behalf.
A Symbian client sits in `SendReceive` until the server answers, so the calling thread blocked
forever. The app was alive and idle, with nothing drawn — a black screen.

Opcode 3 is `akn_icon_server_preserve_icon_data` and opcode 2 `get_content_dim`; both fell into
that branch. So *any* client reaching either one hangs, on any device. The skin value only
decided which client got there first.

Dead end worth skipping: the small `.cre` files look suspicious on size alone — the N95's
`101f876d.cre` is 248 bytes where other devices' are 430–1659, and its `cccccc00.cre` is 21420
against 31441–37634. All of that is just the N95 being an older, smaller S60 3.1 repository set.
A healthy install regenerates byte-for-byte identical sizes. Decode the files; do not weigh them.

## Fix

`services/src/ui/icon/icon.cpp` — the default branch completes with `error_not_supported`. An
unimplemented opcode now returns an error the client can handle instead of stopping it dead.
`preserve_icon_data`, `destroy_icon_data` and `request_to_enable_cache` complete with
`error_none`: this server re-renders from the container file and caches the results itself, so
there is nothing to pin, hand back, or turn on, and those requests are already satisfied.

`system/devices.cpp` — `per_device_storage_paths()` collects everything on disk that belongs to
one device: drive Z, the ROM image, and the central-repository and message-store folders on C, D
and E. `deleteDeviceAtIndex:` deletes all of it, plus the firmware-keyed icon cache under
`Library/Caches/icons/`. Deleting and reinstalling a device now actually resets it.

Where `0x101FD60B` came from is unresolved. It is not in EKA2L1's source, so the guest wrote it;
without a reproduction there is no way to say which code path did. It does not change the fix —
the emulator has no business hanging on a settings value it cannot resolve.

## Verification

- `ekatests`: 109 cases, 499 assertions. Five new ones cover the per-device path list, its
  lowercasing, a delete that must leave a second device and shared state untouched, a device
  that never wrote anything, and the AknIconServer opcode numbering (a wire contract now that
  three opcodes are answered without dispatching).
- The delete test was checked by mutation: dropping `private/10202be9/persists/` from the list
  fails it.
- Release simulator build of the iOS target.

## A warning about `devicectl`

The app container on the test device was destroyed during this investigation by

```
xcrun devicectl device copy to ... --destination '<a subfolder>' -r true
```

`--remove-existing-content` does not scope itself to the destination path. The command failed
with `Failed to retrieve the file node` *after* wiping the entire `appDataContainer` — ROMs,
drives, installed apps and saves. Never pass it against a device you cannot afford to re-image.
Pushing a zero-byte file over a single file is safe and is enough to disable a persist, since
`load_repo_adv` skips empty ones.
