# Warhammer: distinguishing card protection from an emulator crash

Upstream issue #883 reports a startup crash in Warhammer 40,000: Glory in
Death v0.6.9. The supplied original-card ZIP terminates with `KERN-EXEC 3`
before reaching its title screen. A separately supplied RAR of the same
version runs in the same emulator and ROM. Comparing those packages provides
a useful positive control for diagnosing protected N-Gage software.

## The crash is downstream of a failed protected initialization

The faulting instruction is `ldr r2, [r1, #0x240]` at preferred address
`0x10156448`. Its caller passes an already-invalid object pointer. With the
original ZIP and the default zero CID, emulator-side tracing finds the actual
bad write earlier, at `0x1013B7F4`: a valid object pointer is replaced with
`0x03995360` inside the obfuscated function at `0x1013AEB0`. The later
constructor dereferences that result and faults at `0x039955A0`.

Both N-Gage NEM-4 and N-Gage QD RH-29 reproduce the startup failure. Dyncom
and Dynarmic reproduce the same faulting instruction. Disabling the shipped
Warhammer slow-loading hooks does not resolve it. Read-only folder mounting
and writable archive mounting also fail in the same path.

The filesystem is not dropping the protection data in this reproduction.
The reads of `cis.dat`, `nc.dat`, `cwivenc.dat`, `cwp.dat`, and the executable
complete at the expected offsets and lengths. A temporary IPC probe compares
the complete read buffers with the guest descriptor destinations immediately
after writeback; they match, including the 163,964-byte `cwivenc.dat`.

## What the working RAR changes

The RAR contains a small BiNPDA launcher as `6R92.APP` and puts the actual
game in `bin/main.dll`. That DLL differs from the original ZIP executable in
only eight bytes of the E32 creation timestamp. Its entire code section is
identical, including the version hash used by the existing compatibility
hooks. This is a particularly strong control: successful execution does not
require a different game implementation.

The launcher intercepts `RLibrary::Lookup` and substitutes an implementation
for the `RBusLogicalChannel::DoControl` function obtained from EUSER. Its
replacement supplies synthetic MMC card information. The RAR also has a
different 16-byte `nc.dat`, 32 changed bytes near the start of `cis.dat`, and
three changed bytes in `game.lic`.

Capturing the card information returned by that launcher makes it possible
to test the normal emulator MMC interface independently of the launcher.
With the RAR's protection data and that CID, the game runs directly as an
application through the ordinary MMC driver. The protected initialization
receives the same key as in the working RAR and gets past the bad-pointer
write.

## The decisive single-file control

The following comparisons use the same Release emulator source and RH-29 ROM:

| Card contents | CID | Result |
|---|---|---|
| Original RAR and its launcher | Configured default | Reaches title screen |
| RAR game run directly, with card-identification files supplied | Default zero CID | Startup access violation |
| Same direct game and files | CID captured from RAR launcher | Passes protected initialization |
| Original ZIP, unchanged | CID captured from RAR launcher | Same startup access violation |
| Original ZIP with only `nc.dat` copied from the RAR | CID captured from RAR launcher | Reaches main menu |
| Same single-file replacement, accessed through a directory alias without CID metadata | Default zero CID | Same startup access violation |

The last comparison retains the ZIP's executable byte-for-byte, including its
timestamp, as well as its original `cis.dat`, `game.lic`, and game resources.
The negative alias points to exactly the same files as the passing card;
only its automatically selected CID differs. Both outcomes were repeated
after removing the probes and installing a clean Release build.

No launcher or executable patch is needed. The changed license data and
matching card identity are sufficient to get past the reported crash.

The untouched RAR was also mounted directly as an archive in the final clean
Release build on NEM-4 and reached its title screen with the default CID
configuration. The simulator ran with JIT disabled.

The original ZIP does not include its original-card CID. Its `nc.dat` cannot
simply be paired with the synthetic identity used by the other package.
The observed failure is therefore a protection-data/card-identity mismatch
in this setup, not evidence that the constructor, MMC layout, or CPU needs a
Warhammer-specific workaround. This does not establish that the original
ZIP is damaged. A subsequent investigation below recovers an equivalent
identity, without uniquely recovering the original card's full CID.

## Recovering a usable identity for the original ZIP

A later investigation recovered a functional CID without replacing `nc.dat`.
In EKA2L1's four-word configuration format, one working value is:

```
00000000-0e000000-0b3f1959-00000400
```

This is an equivalent identity for the game, not the complete original card's
unique hardware CID. The game copies only CID bytes 14 down to 7. The other
eight bytes above were filled with zero, and the protected key computation
also admits collisions among the bytes it does consume.

The recovery uses the game's own decoder, rather than guessing the original
128-bit register. Function `0x10111098` builds the filename `cwivenc.dat`.
The caller passes those tables and the 16-byte `nc.dat` to `0x10139E38`.
Capturing its decoded output exposes the inputs to the bytewise combination
at `0x10136330`. Executing that combination offline with ARM emulation
reproduces the working RAR's initialization key exactly. Enumerating each
CID byte against that known-good key then yields an input byte sequence
`00 04 00 0B 3F 19 59 0E`, in the game's reverse-copy order. The configuration
value above supplies those bytes through the ordinary MMC driver.

With all original ZIP contents retained and the probes removed, mounting an
extracted, read-only card folder with this identity reaches the title screen
on RH-29 and NEM-4. All 128 extracted files were compared byte-for-byte
with the archive. No launcher, executable, license, or resource replacement
is used.

With a writable archive mount, the game gets past the original fault but
later takes a read/write open of `wh40kECP.cwa` and faults at `0x101100CC`,
reading `0xC9C`. The cached CWA still matches the original file byte-for-byte.
The reason for that writable-path failure has not been established. It must
not be confused with the original invalid-key failure at `0x10156448`.

iOS archive mounts now use the same write protection as folder mounts and
retain it across guest relaunches. The unchanged ZIP reaches the title screen
on NEM-4 with the recovered identity supplied either by its filename or by
the MMC ID setting; the latter also reaches the main menu. All 128 extracted
files still match the archive.

## Two diagnostic traps

Generic Symbian 6.1 SDK `d_mmcif.h` describes a larger `TMmcCardInfo` with
status fields before CID. This game's N-Gage driver contract is different:
runtime code at `0x10148E08` initializes exactly 20 bytes, puts the media type
at offset 16, and reads CID bytes 14 down to 7 after control 6. Substituting
the generic SDK structure overwrites the caller's stack. The observed
20-byte contract must be preserved.

Software breakpoints can also perturb protected, self-modifying code and
produce misleading invalid-instruction failures. The useful traces here
were placed in emulator block dispatch and filesystem writeback, without
modifying the guest instructions. All such probes were removed after the
comparison; no emulator behavior change is required by this investigation.
