# Let's Fly: IAP startup dependencies and cross-process communication

Let's Fly (`0x2006D6C3`) on the X7 RM-707 ROM initially stopped at the
DaSuppa splash with `KERN-EXEC 3`. Installing its missing Nokia In-App Purchase
runtime exposed two independent emulator bugs before the main menu could open.

The faulting PC was `0x10`, reached through the game's import veneer for ordinal
16 of `iapclientapi.dll`. The library was absent from the ROM and installed
drives, so the veneer still contained its import ordinal. This is a missing guest
runtime dependency. The game was subsequently tested with the original Nokia
IAP 0.3.1 installer; no game executable or purchase result was patched.

## The IAP engine could not start its local server

Qt's Symbian socket engine sets `KSONonBlockingIO` before opening its local TCP
server. The Internet backend rejected that option. After a diagnostic allowed
initialization to continue, Qt also blocked on the unimplemented `RSocket::Info`
request, reformed opcode 143.

The TCP backend now tracks nonblocking mode, reports its protocol description,
and implements readiness polling and asynchronous select notification. Empty
nonblocking receives and accepts return `KErrWouldBlock` instead of remaining
pending; nonblocking TCP writes use the available native send capacity. Async
connect readiness supports Qt's second `Connect` call after notification.
Cancellation detaches select requests under the kernel lock, and their native
timer cleanup retains the request independently of the socket's lifetime.

The relevant contracts are in Symbian `ES_SOCK.H`, `SOCKMES.H` and `CS_CLI.CPP`;
Qt's `qsymbiansocketengine.cpp` shows how initialization, synchronous waits and
select cancellation depend on them.

## A ready engine still did not wake the client

Once its server was listening, the engine became idle while the game remained
blocked on its startup semaphore. Inspecting the kernel object table showed two
distinct objects with the identical global name
`qipc_systemsem_IAPenginesemaphore32cd1fafa21ceb598b75b73ebb79096ce5612f5e`.
One process signaled its own object while the other waited on a different one.

EKA2 semaphore creation now returns `KErrAlreadyExists` for an existing nonempty
global name. Qt then opens the existing semaphore, as its create-or-open sequence
expects. Symbian's `ExecHandler::SemaphoreCreate` and the named-object creation
contract establish this behavior. The EKA1 path already had an equivalent check.

## Verification

Release simulator testing on revision `f1101ac1d`, using RM-707
and JIT disabled, reached the main menu, aircraft selection, mission prompt and
live flight. Flight advanced, coins accumulated and the pause menu responded.
The default simulator suite passed 12/12 checks and the X7 Angry Birds HID suite
passed 5/5. Host socket tests passed 68 assertions across two cases, covering
nonblocking reads, protocol information, readiness and cancellation before and
after native select registration, as well as connection refusal and retries.
The final audit also verified IPC numbers and parameter slots directly against
the X7 ROM exports. Named-semaphore uniqueness uses folded-case comparison,
and select completion validates the requesting thread identity.

The game uses device tilt for steering. Physical steering and sound remain
unverified; the configured iPhone Air was unavailable during this investigation.
