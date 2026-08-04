# LAN netplay discovery crashes at startup on every getifaddrs platform

## Symptom

With `btnet-discovery-mode: 2` (LAN) in `config.yml`, the iOS app died a second or two
after launch, before the app list was ever drawn. `EKA2L1.log` stopped at exactly the
same byte every run:

```
E .../config/src/panic_blacklist.cpp:33 [Config]: Failed to open thread panic blacklist file!
```

which is just the last line of ordinary startup — nothing pointed at networking.

## Narrowing it down

Two dead ends worth skipping next time:

- There was no crash report to read. `~/Library/Logs/DiagnosticReports` did not exist,
  and the simulator device's own `Library/Logs/DiagnosticReports` was empty.
- `simctl spawn booted log stream` drowns the interesting moment in CoreFoundation and
  CoreAudio chatter and simply stops emitting once the process dies, so the last lines
  it prints (CoreMotion init, in this case) are a coincidence, not a lead.

What worked was attaching a debugger to the launch:

```sh
PID=$(xcrun simctl launch --wait-for-debugger booted com.eka2l1.emulator | grep -o '[0-9]*$')
xcrun lldb -b -p $PID -s <(printf 'continue\nthread backtrace all\nquit\n')
```

That produced the real stack immediately:

```
EXC_BAD_ACCESS (code=1, address=0x1)
  host_sockaddr_to_guest_saddress(addr=0x0, ...)          resolver.cpp:102
  inet_socket_interface_iterator::next(...)               socket.cpp:1343
  epoc::internet::retrieve_local_ip_info(...)             socket.cpp:714
  epoc::bt::midman_inet::setup_lan_discovery()            btmidman_lan_matching.cpp:26
  epoc::bt::midman_inet::midman_inet(...)                 btmidman_inet.cpp:95
```

`host_sockaddr_to_guest_saddress` dereferences `addr->sa_family` with no null check, so
`addr == nullptr` faults on the `sa_family` offset — address `0x1`.

## Root cause

The POSIX branch of `inet_socket_interface_iterator::next()` walked the `getifaddrs`
list filtering only on interface name (`vmnet*`) and `IFF_RUNNING`, then unconditionally
converted `ifa_addr`, `ifa_netmask` and `ifa_broadaddr`.

`getifaddrs` also returns one **link-layer** entry per interface — `AF_LINK` on
BSD/Apple, `AF_PACKET` on Linux — carrying the MAC address and *nothing else*: both
`ifa_netmask` and `ifa_broadaddr` are `NULL`. On macOS/iOS the very first entry returned
is `lo0`'s `AF_LINK` record, and it is `IFF_RUNNING`, so the crash was deterministic and
happened on iteration one. Point-to-point interfaces (`utun*`) are a second case: real
`AF_INET6` addresses, but no broadcast address.

This was never iOS-specific — any `getifaddrs` platform hits it. It only surfaced now
because LAN discovery is the one caller that runs unprompted at emulator startup, so
selecting discovery mode "LAN" in settings turned it into a launch crash.

A second latent bug sat in the same loop: the skip loop can run off the end of the list,
after which `current_addr_info_posix->ifa_name` is dereferenced through a null pointer
instead of reporting EOF.

## Fix

In `inet_socket_interface_iterator::next()` (`socket.cpp`), the skip loop now also
rejects entries with a null `ifa_addr` or a family other than `AF_INET`/`AF_INET6`, and
returns `error_eof` when the list is exhausted. `ifa_netmask` and `ifa_broadaddr` are
null-checked individually, zeroing the corresponding guest address and length instead of
faulting; the Android branch that derives the broadcast address from `addr | ~netmask`
is skipped when there is no netmask to derive it from.

After the fix the emulator boots to the app list with LAN discovery enabled and
`setup_lan_discovery` no longer logs "Can't find local LAN interface for BT netplay!",
i.e. it now picks up the real IPv4 interface it was crashing before it could reach.
