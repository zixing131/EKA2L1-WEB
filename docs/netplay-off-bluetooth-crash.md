# Two-player mode killed the emulator when netplay was switched off

## Symptom

On an N-Gage (NEM-4), Dragon World's *Double Game → As Client* and *As Server* took the
whole app down every time. The emulator log simply stopped after the last resource file
of the two-player screen (`e_waitconnect.pic`); the TestFlight reports all pointed at the
same place:

```
Thread 13 Crashed:
0  uv_timer_stop + 0
1  eka2l1::epoc::bt::midman_inet::reset_friend_timeout_timer()::$_0::operator()()
2  libuv::looper::tasks_handle_function()::$_0::__invoke(uv_async_s*)
...
EXC_BAD_ACCESS (SIGSEGV) KERN_INVALID_ADDRESS at 0x0000000000000070
```

The user's only relevant setting was **Bluetooth netplay discovery mode = off**; every
other mode played fine.

## Root cause

`midman_inet`'s constructor returns early when the discovery mode is `off`, before it
creates any libuv resource. `hearing_timeout_timer_` therefore stays a null
`shared_ptr`, and `uv_timer_stop` dereferences it at offset 0x70 — hence the fixed fault
address in every report.

Nearly every public entry point of `midman_inet` already opens with a
`discovery_mode_ == DISCOVERY_MODE_OFF` early return for exactly this reason. Two did
not: `begin_hearing_stranger_call()` and `unregister_stranger_call_observer()`. The
Bluetooth host resolver never exposed the hole because it guards its own calls with
`get_discovery_mode() > DISCOVERY_MODE_DIRECT_IP`. The device-selection notifier plugin
(`RNotifier` UID `0x100069D1`, which is what the game uses to pick a peer) had no such
guard, so it registered an observer, which posted `send_call_for_strangers()` onto the
loop, which posted `reset_friend_timeout_timer()`, which stopped a timer that was never
created.

Behind that sat a second, independent failure on the *As Server* path. With discovery
off, `get_free_port()` also returns 0, so the RFCOMM "give me a free server channel"
ioctl answers 0 and `bind()` fails with `KErrEof`. Once the host crash was gone, the
guest instead died with `USER 0`. Virtual Bluetooth ports are nothing but a local
bitmap, so refusing to hand them out is over-strict: netplay off means "no peers", not
"no Bluetooth stack".

## Fix

* `begin_hearing_stranger_call()` / `unregister_stranger_call_observer()` are no-ops
  when discovery is off, matching the rest of the class, and the timer-reset task bails
  out on a null handle.
* The device-selection notifier answers `KErrNotFound` right away when discovery is off,
  the same answer a search that finds nobody produces. Answering there matters: the
  midman cannot report "no strangers" itself without a running loop, and the guest
  request would hang forever.
* The virtual port allocator (`get_free_port`, `ref_port`, `ref_and_public_port`,
  `close_port`) now works in every mode; `port_refs_` / `port_upnp_mapped_` are
  initialized before the constructor's early return. `get_host_port()` keeps returning 0
  when discovery is off, so the socket falls back to an ephemeral local bind and no port
  is published or UPnP-mapped.
* The random virtual Bluetooth address moved above the early return too. `device_address`
  is a POD with no initializers, so with discovery off `local_device_address()` — read by
  `btinet_socket::local_name()` — was handing the guest uninitialized memory. Nothing
  reached that far while `bind()` still failed; letting the allocator work made it
  reachable.

## Result

With discovery off on NEM-4: *As Client* returns to the menu (no peer found) and
*As Server* sits at "waiting connection…", the same screen direct-IP mode reaches.
Direct-IP mode is unchanged and still publishes 15000/15020 as before.

## Worth knowing

* The crash reproduces on the simulator — it is host-side C++, not device-specific.
  Reproducing it first on an unpatched build (`git stash`) and comparing the decoded
  `.ips` against the TestFlight report is what proved the diagnosis rather than
  suggesting it.
* Guest panics still do not reach `EKA2L1.log` (panic lines are `LOG_TRACE` and spdlog
  flushes at `debug`), so the `USER 0` above was only visible in the on-screen dialog.
  The `Bluetooth ports have ran out. Can't bind!` error line was the trail that led to
  it.
* After relaunching the app, `ui-automation` element refs must be re-snapshotted before
  tapping; stale refs silently do nothing and make the guest look unresponsive.
