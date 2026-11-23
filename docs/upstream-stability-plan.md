# A stability net for upstreaming this fork

This fork carries several hundred commits' worth of change over upstream `master`, and the
intent is to send it back in batches. Most of it touches shared emulator code — kernel,
IPC, services, the interpreter — where a regression is invisible until somebody runs the
one application that used to work. Upstream had no automated way to notice that.

So the batches should not start with behaviour. They should start with the net that
catches a bad batch. That net is now in place: #583 brought CI back from the dead, #584
made it run the unit tests, #586 got the whole thing onto current toolchains and runner
images, #585 fixed a startup crash found while checking that the macOS artifact opens, and
#588 gave branch protection one stable check to require. This document is the plan for
that net: what exists, what PPSSPP (the emulator project with the most mature regression
story) actually does, why EKA2L1 cannot copy it verbatim, and the concrete order in which
to build it.

Two things have changed since the plan was written, and both are recorded in place below:
the net's first two rungs are built and merged, and **upstream accepted the iOS port
itself (#587, 17 Aug 2026)** — which was the open question the whole "iOS plumbing" batch
was waiting on. See [What is left in the fork](#what-is-left-in-the-fork), remeasured on
21 Aug 2026, after #605 and #606 closed out the interpreter and the graphics batches.

## Where upstream stands today

*This section describes upstream as of 16 Aug 2026, before A0 landed. It is kept as the
starting position the rest of the document is measured against; the per-step statuses say
what has since changed.*

- `.github/workflows/build.yml` does trigger on `push` and `pull_request` — and **it has
  been failing on every single run**, in a way that is easy to miss: each job dies in
  *Set up job*, before one step executes. GitHub hard-fails any workflow referencing
  `actions/upload-artifact` v1/v2 or `actions/cache` v1, and this one references both. Run
  31083801253 (master, 2026-08-06) is the last one, and every retained run before it, back
  to January 2026, failed too. Nothing has been built by CI for months, and PR
  authors have been looking at a red X that says nothing about their change.
  The rest of the workflow rotted alongside it: `actions/checkout@v2`, the removed
  `::set-output` syntax, the retired `ubuntu-20.04` image, `macos-latest` now being arm64
  (which open-source Qt 5.15.2 has no binaries for), signing and release actions pinned to
  Node versions Actions no longer runs, and a Windows OpenSSL download from a host that no
  longer resolves. "Turn CI back on" is really "rebuild CI".
  *(Note for anyone reading this in the fork: the fork's own copy was switched to
  `workflow_dispatch` so it would stop firing here. Upstream's is not.)*
- `EKA2L1_BUILD_TESTS` defaults to `ON` and `src/tests/CMakeLists.txt` registers
  `ekatests` with CTest — but the workflow builds only the `eka2l1_qt` target and never
  invokes `ctest`. The unit tests are configured, never built, never run.
- What that costs is already documented: see
  [Restoring the `ekatests` build](./ekatests-build-and-runtime-failures.md). Bringing the
  target back to life required fixing a link failure, a cross-build header collision, a
  stale call signature, and then **eight failing assertions that turned out to be three
  real bugs** in `bitmap_allocator`, `basic_pystr::as_int()`, and the directory iterator.
  Those are shipped bugs in shared code, found the moment a dormant test target was run.
- `src/intests` is already the right shape for guest-level regression testing: a Symbian
  test application plus a checked-in `expected/` tree covering Chunk, IPC, CodeSeg,
  EComServer, FbsBitmap, FbsFontStore, FileIO, WindowServer, CmdAndParams. But per its
  README the whole loop is manual — build the SIS, install it, run the app, read
  `DebugPrint` by eye. Nothing diffs it, nothing gates on it.
- There is no headless frontend. `src/emu` has `qt`, `android`, `ios` and nothing that a
  script can drive.
- Assets this fork can contribute immediately: `dyncom_difftest` (a host-only,
  ROM-free differential harness for the interpreter, driven by `scripts/cpu_difftest.sh`)
  and `scripts/ios_regression_test.sh` (screenshot assertions with pixel-difference
  thresholds against real guest applications).

## What PPSSPP actually does

Verified against the current tree, not from memory. `hrydgard/ppsspp`'s
`.github/workflows/build.yml` triggers on `push` (master and tags) **and
`pull_request`**, across a matrix of Ubuntu (clang and gcc), Android (arm64/arm32/x86_64,
plus VR and libretro), macOS, iOS, Windows (x64 and ARM64) and LoongArch. Every
configuration then runs up to three layers of tests:

1. **Unit tests** — `./PPSSPPUnitTest ALL`. Pure host-side logic, no emulation.
2. **Headless golden-output tests** — `python test.py -g --graphics=software`. `test.py`
   feeds ~300 `.prx`/`.elf` homebrew tests from the `pspautotests` submodule into
   `PPSSPPHeadless --root <path> --compare --timeout=5` over stdin; the emulator diffs the
   guest's own output against a checked-in `.expected` file per test. Tests are bucketed
   into `tests_good` (must pass, gates CI), `tests_next` (known-broken, informational) and
   `tests_ignored`. **Software rendering is the trick that makes this deterministic on CI
   machines with no GPU.**
3. **Frame-dump replay** — `frametests.py` replays recorded `.ppdmp` GE command streams
   through the headless binary, renders an image, and compares it to a checked-in
   reference by **mean squared error** against a `maxMse` tolerance, emitting an HTML
   report with the images and diffs.

Outside CI, PPSSPP closes the loop with user-side compatibility reporting
(report.ppsspp.org) and a per-title `compat.ini` of behaviour flags.

The load-bearing fact for us: **pspautotests is homebrew PPSSPP wrote itself and can
redistribute.** That is what lets guest-level tests live in public CI.

## The constraint that forces two tracks

EKA2L1 cannot run a single line of guest code without a Symbian ROM, and ROMs are
copyrighted. They cannot be committed, cached, or handed to a fork's pull request (which
by design gets no repository secrets). Any plan that puts guest tests in the public PR
path is dead on arrival.

Hence two tracks, with different triggers and different owners:

| | Track A — public CI | Track B — ROM-gated |
|---|---|---|
| Trigger | every push and pull request, forks included | maintainer-initiated / self-hosted |
| Needs | nothing but the checkout | a ROM, a mounted device, installed applications |
| Catches | build breakage, host-layer logic bugs, CPU semantic drift, memory errors | kernel, service, IPC and rendering regressions in real guest behaviour |

Track B **cannot** run on fork pull requests. That is a property of GitHub's secret model,
not a policy choice, and any proposal should say so up front so it is not mistaken for a
demand that every contributor own a ROM.

## Track A — four steps, each a standalone PR

### A0. Make the workflow run again

The triggers are already right; nothing about them needs changing. What needs changing is
everything the workflow depends on: actions bumped to v4, `::set-output` replaced with
`$GITHUB_OUTPUT`, retired runner images replaced (`ubuntu-24.04`, and `macos-15-intel` to
keep the x86_64 Qt 5.15.2 build that arm64 `macos-latest` cannot provide), the Windows
OpenSSL download dropped because the host serving it no longer resolves and Qt has no
replacement package, APK signing done inline with `apksigner` instead of an unmaintained
action — and, so that fork
pull requests are not red by construction, signing skipped with an unsigned APK uploaded
when the secrets are absent. `fail-fast: false` stays, so one platform's failure still
reports the others. Rolling releases stay restricted to a push on `master`.

No test steps are added here. The point is only to make "every PR has a signal" true again,
on infrastructure that still exists.

*Acceptance:* a green build on every matrix entry.

**Status: done and verified**, on branch `ci/modernize-build-workflow` (based on upstream
`master`, five commits). Four rounds on this fork's Actions were needed, because each fix
uncovered the next failure — worth recording, since it is what "CI has not run in months"
actually costs:

1. Deprecated actions replaced → jobs finally reach a step.
2. Linux missed `qtbase5-private-dev` (`displaywidget.cpp` includes the Qt private header
   `qpa/qplatformnativeinterface.h`); macOS could not configure because CMake 4 refuses
   capstone's `cmake_policy(SET CMP0048 OLD)`; `tools_openssl_x64` no longer exists.
3. CMake pinned to 3.31.6 and Windows moved to `windows-2022` — the vendored dependency
   tree cannot be configured by CMake 4 at all (capstone's policy, plus glm, fmt, spdlog
   and dynarmic declaring a minimum below 3.5), and CMake 3.31 predates the Visual Studio
   2026 that `windows-latest` switched to in June 2026.
4. macOS then failed to compile: the bundled boost uses `std::unary_function`, which
   libc++ no longer declares, and miniBAE's old C trips `-Wint-conversion` and
   `-Wimplicit-function-declaration`, errors by default since Clang 16.

The last two are the only source changes; everything else is workflow-only. All four jobs
are green and produce artifacts (run 31935443699 on this fork), and the Android job was
confirmed to take the unsigned path when no signing secret is present. The change went
upstream as PR 583, where the `pull_request` run is green on all four jobs and
`roll-release` correctly skips.

Two paths could not be exercised from a fork — the signing branch needs the secrets, and
`roll-release` only runs on a push to `master`. Both were settled when upstream merged the
PR: that push signed the APK with `apksigner` (build tools 37.0.0), moved the `continous`
tag to the merge commit, and refreshed all four release assets.

Two things are deliberately left for the maintainer to decide:

- **Windows has no TLS backend.** Qt retired `tools_openssl_x64` with OpenSSL 1.1, and
  Qt 5.15.2 cannot load the OpenSSL 3 package that replaced it, so the in-app updater
  cannot reach api.github.com. Shipping an unusable library instead would only hide it.
  The real fix is a Qt bump.
- **The CMake and Visual Studio pins are a holding action.** They should come off once the
  vendored submodules configure under CMake 4.

### A1. Fix `ekatests`, then run it in CI

Split the repair described in
[the ekatests write-up](./ekatests-build-and-runtime-failures.md) out of this fork as its
own PR: generated `configure.h`/`version.h` moved into each target's binary include
directory (so iOS, desktop and test build trees stop overwriting each other's feature
configuration), headless frontend stubs owned by the test target, the stale applist call
signature, and the three shared-code bug fixes the restored tests exposed. Then add
`--target ekatests` and `ctest --output-on-failure` to the workflow.

This is the highest-value single step in the plan, and it doubles as the argument for the
rest of it: the fixes *are* the evidence for what a dormant test target costs.

*Acceptance:* `ctest` green on all desktop platforms; a deliberately reverted bug fix
turns it red.

**Status: done and verified**, on branch `ci/ekatests-in-ci`, squashed into one commit
covering the build repair, the three shared-code bugs and the CI wiring. Ported onto upstream's tree by
building there and following the failures, not by cherry-picking the fork's commit; the
`as_int()` result is byte-identical to the fork's, and each headless stub was checked
against upstream's own declaration.

Both halves of the acceptance criterion hold: `ctest` reports `100% tests passed` on
Windows, macOS and Linux, and reverting only the allocator fix turns all three red with
`64 test cases | 60 passed | 4 failed`. That second run also answers a question the first
cannot — the suite really executes, rather than matching zero tests and exiting 0.

Two things the port surfaced that are not in the fork's commit:

- On Windows the test executable is not built next to the runtime DLLs the frontend build
  copies into `bin`, so `ctest` failed with `0xC0000135` until the test's `PATH` was
  pointed at them, and the fixtures are copied next to the executable, which on a
  multi-config generator is not where CTest runs from.
- The suite cannot be built on a current macOS toolchain at all: SDK 26 no longer declares
  `stat64`, which `fileutils.cpp` uses on every POSIX target. CI did not catch it because
  its image is older. *Since fixed upstream — `fileutils.cpp` now maps `stat64` onto
  `stat` on Apple targets, and `ekatests` builds and passes on a current macOS SDK.*

Upstream merged A0 as PR 583, so this went out as a standalone PR rather than a stacked
one.

### A1b. Modernise the toolchain (unplanned, but it blocked everything else)

Not in the original plan: the pins #583 had to introduce — CMake 3.31, Visual Studio 2022,
an Intel macOS runner — were holding CI on retiring ground, and the tree did not build on a
current toolchain at all. Sent as #586: capstone/fmt/spdlog/glm bumped, the policy floor
relaxed for the vendored subtree instead of pinning CMake, Qt 6 (which the frontend already
supported, and which gives Windows its TLS backend back), all three runners on `latest`,
arm64 ffmpeg built from the submodule's own script, and Discord's Game SDK replaced with
discord-rpc so nothing prebuilt or network-fetched is left in a build.

Checking that the macOS artifact actually opens turned up four more: a signature invalidated
by the bundle fixups (Apple Silicon kills the process), the Qt platform plugin never copied
under Qt 6, an SDL2 framework copied in a way codesign rejects, and guest memory commits
failing on 16 KB host pages under W^X. A fifth, a null dispatcher when no device is
installed, was not architecture specific and went out separately as #585.

*Status: merged.*

### A2. Run the interpreter differential harness in CI

`dyncom_difftest` needs no ROM and no GPU, so it belongs in Track A as-is. Fixed-seed
short run on every PR (tens of seconds); long randomised run nightly, seed printed so a
failure is reproducible. Without this gate, none of the interpreter optimisations in this
fork (Thumb de-indirection, translation-time bulk-loop acceleration, the LDM/STM block
cursor) can be upstreamed responsibly.

*Acceptance:* the harness exits non-zero on the first divergence and the failing seed is
in the log.

**Status: merged (#604).** It did not go out on its own in the end: the harness, the CI job
and the interpreter batch it gates travelled together, because the batch is the only thing
that makes the harness's coverage arguable and the harness is the only thing that makes the
batch reviewable. The job is `dyncom-difftest` on `ubuntu-latest`, running
`./scripts/cpu_difftest.sh 1 20000` and feeding the `ci` aggregate check, so branch
protection covers it alongside the build jobs. The harness itself grew during review from a
self-A/B plus a golden ALU oracle into two phases: 20000 single-instruction cases against
the golden model, then 544 whole programs against **dynarmic** as an independent oracle,
with coverage counters that assert the loop accelerator and the VFP host-fast path were
actually exercised rather than silently skipped. The nightly long randomised run is still
not wired up.

### A3. A sanitiser job

One Linux clang build with `-fsanitize=address,undefined`, running `ekatests` and the
difftest. Heap overruns and use-after-free dominate this project's bug history — the
inflate tail-word overread that non-deterministically corrupted the host heap during
patch-DLL decompression was found exactly this way, under ASan.

*Acceptance:* the job is red on a known-bad commit and green on its fix.

**Status: sent as #609**, with the five defects its first runs reported (a segment table
allocated with `new[]` and freed with plain `delete`, a negative shift in
`bitmap_allocator::force_fill`, `chunkyseri` offsetting a null pointer to measure a
serialisation, and thirteen descending drive loops stepping one below `drive_a`, which
leaves the enum's value range). The first of those is the argument for the job in one
line: it is invisible on macOS, where ASan turns the alloc-dealloc-mismatch check off by
default, and it took the job's first successful Linux run to surface it. Leak detection is off in the job: the suite ends with
objects the emulator never frees on purpose. The first CI run also turned up that the
vendored `xz` refuses `-fsanitize=` unless `XZ_SANDBOX=no` is passed — Landlock and the
sanitisers cannot both be on, and macOS never hits it.

**Originally: not started, and the last unbuilt rung of Track A.** Cheap next to A2 and
independent of it — one job on an existing workflow, no new target. The argument stopped
being hypothetical while #599 was being prepared: running the existing suite under ASan for
the first time turned up an uninitialised-member defect that reproduces on upstream's own
tree (see
[Sending the first behavioural batch](#sending-the-first-behavioural-batch)). The suite it
would run already exists and already passes under the sanitiser, and #604 added a second
host-only, ROM-free suite the same job can cover: `dyncom_difftest`.

### A4. Grow the unit tests with the batches, not separately

No coverage target. One rule instead: **every shared-code bug fixed in an upstreamed
batch gets a `src/tests` case that reproduces it.** Inflate tail-word reads, allocator
boundaries, region arithmetic, the INI tokenizer's quoted-comma handling, the
`lower_bound` hit that was never validated — all of these are testable with no ROM and no
device. This is also the most direct answer to "how do we trust a large machine-assisted
batch": each fix arrives with a test that fails without it.

**Status: in force, and it is holding.** Every graphics batch sent so far carried its
tests: `loader/nvg.cpp` and `services/ui/skin/skn.cpp` with #589/#591, `loader/svgb.cpp`
with #593, `loader/gdr.cpp` and `services/fbs/linkedfont.cpp` with #594. Upstream's
`src/tests/epoc` now holds 19 test files against the fork's 23, and the rule cost nothing
to follow — the fixtures were synthesised, not extracted from a ROM.

## Track B — guest-level regression, gated on a ROM

### B1. A headless frontend

The prerequisite for everything else here: an `eka2l1_headless` target taking a ROM/device
and an application UID, rendering in software, with a timeout, a meaningful exit code and
optional screenshot output. The contract already exists in miniature — the stubs the
`ekatests` target grew (host UI operations fail cleanly instead of leaving an async
request pending) are exactly what a headless frontend must provide.

### B2. Automate `src/intests` — EKA2L1's `test.py`

The golden outputs are already checked in; what is missing is the driver. Install the SIS
through the headless binary, run the test application, capture `DebugPrint`, diff against
`expected/`, and summarise pass/fail/timeout — with PPSSPP's `good`/`next` bucketing so
known-unimplemented areas stay informational instead of blocking.

This is the closest analogue to what makes PPSSPP's net work, and it aims straight at the
area this fork changed most: IPC argument layout, descriptor slots, and service
completion/cancellation semantics. The built SIS should be committed as a binary artifact
— requiring every contributor to own a Symbian toolchain would make the suite unusable.

### B3. Screenshot regression with an MSE tolerance

A platform-neutral port of `scripts/ios_regression_test.sh`: boot an application headless,
sample frames on a schedule, compare against a reference image by mean squared error or
differing-pixel count. Two lessons from the iOS version are worth carrying over — a
freshly launched guest application needs on the order of 20 seconds before any assertion
is fair, and a single global threshold does not work (a full-screen transition and a
carousel page change differ by an order of magnitude in pixels touched). Reference images
contain no ROM material and can be public; *producing* them needs a ROM, which is why this
sits in Track B.

### B4. Where Track B runs

Two options, and the second costs upstream nothing:

- A self-hosted runner owned by the maintainer with ROMs resident, triggered on `push` and
  on labelled PRs only.
- **A shadow CI on this fork.** Track B runs here, and every upstream PR carries its
  results — the intests summary, screenshots, the relevant log excerpts — in the
  description. For a project with essentially one maintainer this is far more realistic
  than asking them to run each batch themselves, and it needs no upstream change at all.

## What is left in the fork

*Four measures live in this section, oldest first. The current one is
[1 September](#remeasured-1-september-after-659-662); the three before it are kept because
they show what the batches actually removed, not because their tables are still
actionable.*

Remeasured on 2026-08-21, with `master` at the merge of #606. **The earlier count — 162
commits split 91/29/42 — is superseded, and not by arithmetic.** Twenty-three PRs landed
between 16 and 21 August, and none of them was a cherry-pick: each was rebuilt on
upstream's tree and reviewed there, so a fork commit's content can be fully upstream while
the commit itself still shows as "ahead". Commit counts stopped being a valid unit of
measurement the moment that started happening. What follows is measured with
`git diff master...ios-next` — by file and by line, which is what actually still differs.
`docs/` and `src/emu/qt/translations/` are excluded throughout.

Five things closed out entirely:

- **The memory model batch is upstream (#599).** Chunk creation, chunk lifetime across
  processes and CPU TLB invalidation, plus a defect that predated the fork. `src/emu/mem`
  is now identical to upstream.
- **The interpreter is upstream (#604, #605)** — and with it A2, since #604 carried the
  `dyncom-difftest` CI job as well as the harness, and #605 then went out behind that gate
  with the three remaining defects (a null addressing function that used to be called, an
  undecodable ARM instruction indexing the translate table with an uninitialised value, and
  Thumb `BKPT` translated as `SVC`). `src/emu/cpu` is now identical to upstream.
- **Audio is upstream (#602, #603).** Stream lifetime, notification delivery, realtime-thread
  safety, and the intro-movie stutter. What is left of "audio and video" is two video files
  that belong to the dispatch/drivers rows.
- **The iOS port is upstream (#587).** `src/emu/ios` differs by 4 files and +69/-7 — the
  font-import and netplay changes made after the PR was cut.
- **Graphics, the window server and the font store are upstream.** #589 (icon decode),
  #590 (NVG path decoding), #591 (Symbian^3 skin overrides and NVG rasterisation),
  #593 (SVGB text), #594 (CJK linked typefaces and imported fonts), #597 (the icon mask
  contract) and finally #606 — window-server redraw and DSA rescale, the FBS font store,
  the screen driver, the AknIconServer contract and the `scdv` patch DLL's C++ source.
  `src/emu/services/src/{window,fbs,ui}` are now identical to upstream. What is still
  graphics work below lives in `dispatch` and `drivers`, which are separate rows.

**Remeasured again on 21 August, after #607-#625 all landed.** Same exclusions. The
table below is the 21 August morning measure; what the row-by-row numbers look like now
that the services batch is in:

| Area | Files | Lines |
|---|---|---|
| Kernel objects and lifetimes | 21 | +862/-162 |
| Netplay and Bluetooth | 23 | +795/-92 |
| Scripting patches | 7 | +448/-63 |
| Common, vfs, utils, system, config | 22 | +396/-66 |
| Patch DLLs | 5 | +342/-0 |
| Dispatch / GLES HLE | 16 | +289/-33 |
| Drivers | 15 | +247/-15 |
| iOS frontend | 4 | +69/-7 |
| Qt / Android frontends | 4 | +23/-65 |
| Services (non-graphics, non-netplay) | 4 | +23/-13 |
| Package, cpu, mem, graphics | 0 | — |

Four areas are at zero now: memory model, interpreter, graphics and package. What is
left of the services row is three things the fork keeps on purpose -- `delete_registry`
public for the iOS uninstall path, the app language following the configured locale, and
the sensor callback's `is_wiping()` guard, which belongs to the kernel batch -- plus the
`CMakeLists.txt` entries for the Bluetooth notifier that goes with netplay.

What remains, by area:

| Area | Files | Lines | Depends on | Verified by | Start with |
|---|---|---|---|---|---|
| Services (non-graphics, non-netplay) | 30 | +1450/-75 | — | `src/intests` once B2 exists | **sent as #610-#625**, one PR per root cause; what is left of the row is the notifier plumbing that belongs to netplay |
| Kernel objects and lifetimes | 21 | +862/-162 | three commits must be unpicked first | `ekatests` + Track B | `e57f9e7e` IPC message refcount (full triage doc, reproducible crash) |
| Netplay and Bluetooth | 21 | +787/-91 | internal order (fixes stack on earlier work) | two-instance manual test | whole batch, in fork order |
| Common, vfs, utils, system, config | 25 | +526/-74 | — | `ekatests` | `flate.cpp` (+18) — the inflate tail-word overread; needs a test written for it (A4) |
| Scripting patches | 7 | +448/-63 | upstream's scripting build state | Track B | `1093f038` resolve ROM hooks by fingerprint |
| Package | 6 | +439/-148 | — | `ekatests` (SIS fixtures exist) | **sent as #607** |
| Patch DLLs | 5 | +342/-0 | a Symbian toolchain to rebuild the binaries | Track B | — |
| Dispatch / GLES HLE | 16 | +289/-33 | — | Track B, per device | — |
| Drivers (graphics and camera backends) | 15 | +247/-15 | — | Track B, per device | — |
| iOS frontend | 4 | +69/-7 | — | device build | whole remainder, one PR |
| Qt / Android frontends | 4 | +23/-65 | — | desktop run | — |
| Graphics, window server, fonts | 0 | — | — | — | **done** (#589–#597, #606) |
| Interpreter (dyncom) | 0 | — | — | — | **done** (#604, #605) |

The whole services row went out on 21 August as sixteen PRs, #610 through #625,
split by root cause rather than by file: three touch the IPC framework itself
(object lookup by id, null descriptor arguments, completing requests nothing
implements), three the file server, and the rest one service each -- etel, alarm,
featmgr, loader, accessory, applist twice, sensor, msv, the time zone HLE and the
disk-space properties.

Rebuilding them on upstream turned up things the fork had wrong, which is the
argument for the rebuild rule stated again:

- **A feature id was mislabelled.** The fork enabled featmgr id 1012 under the name
  "app menu show images". Symbian's `publicruntimeids.hrh` says 1012 is
  `KFeatureIdHelp`. The fix works either way -- AVKON adjusts a menu pane on it --
  but the PR now names and explains it correctly.
- **`applist`'s duplicate-registration guard covered one of the two insertion
  paths.** The fork patched the EKA1 loader; the modern one still appended blind.
- **The sensor callback fix depended on `kernel::is_wiping()`**, which lives in the
  fork's kernel batch. The session-pointer check alone carries it upstream.
- **The time zone tests needed a portable way to pin `TZ`.** MSVC has no `setenv`,
  and its CRT takes a POSIX TZ string rather than a zone name, so the
  transition-shape test is Linux and macOS only.

#607, #609 and #610 are the batches sent after this remeasure. Two notes on the last two:
the sanitiser job had to carry its own fixes, because a gate that is red on the day it
lands is not a gate; and the time zone HLE could not go out alone, since the two
`common/time.cpp` helpers it calls have no other caller upstream and would have been dead
code in a PR of their own. Both are the same judgement call as #599's bundling.

#607 is the first batch sent after this remeasure. Rebuilding it on upstream turned
up four defects the fork's own version had, which is an argument for the rebuild
rule on its own: the guard meant to keep uninstall away from ROM files compared
`towlower(target[0])` against `drive_to_char16(drive_z)`, which returns an
uppercase letter, so it never fired; `package::object::in_rom` had no initialiser,
so the flag a new refusal reads was stack garbage on the SIS v1 path, both for a
fresh install and for every registry that path had already written; and the
lowercase fix for case-sensitive hosts turned out to be untestable on macOS
(case-insensitive FS) until the suite was run on a case-sensitive volume, where it
fails six cases. The fork still carries all four; they come back with the next
merge from master.

The netplay row counts `services/{bluetooth,socket,internet}` plus the Bluetooth notifier;
the services row is everything else under `src/emu/services`. Earlier measures split those
two differently, so compare their sum (51 files, +2237/-166) rather than either row.

Three areas are deliberately not rows above, because none of them is a batch:

- **`src/tests`** — 19 files, +873 lines. Under A4 each case travels with the batch whose
  bug it reproduces, so this is not a PR of its own. It is also the one number that should
  *shrink to zero* as a side effect of everything else going out.
- **Build, CI, scripts and top-level files** — 19 files, +2447 lines, and almost all of it
  is iOS-specific infrastructure that lives in this fork by design: the TestFlight and
  signing workflows, `ios_regression_test.sh`, the MetalANGLE fetch, `AGENTS.md`. Only the
  parts upstream would actually run belong in a PR.
- **`docs/`** — 120 files of per-bug write-ups, which are PR material rather than a batch.

Three things the table does not say on its own:

- **Two areas have now reached zero, and both did it the same way.** `src/emu/cpu` went
  out as #604 (the optimisations, carrying A2) and #605 (the three defects the
  optimisation PR deliberately left behind), each behind the gate the previous PR had
  installed. Graphics went out as seven PRs over six days, ending with #606. The pattern
  worth repeating: send the gate first, then send the behaviour that the gate covers, and
  keep bug fixes out of a PR whose subject is performance.
  Writing #605's cases turned up a defect in the harness itself: without scripting
  enabled `COND_CHECK` does not guard `log::filterings`, so any path under test that
  logs took the harness down — which is why nothing had ever reached those paths.
- **Some commits have to be split.** `b1153e25` (a batch of TestFlight crashes) and
  `061cc4d3` (four ThreadSanitizer races) each carry several unrelated root causes, and
  both sit in the kernel row. One root cause per PR means unpicking them first, so the
  file counts above are not PR counts either. Two other split candidates —
  `4a9f96d3` (fbs allocator race plus two teardown UAFs) and `2a53883f` (an NVG icon
  abort plus a memory-model UAF) — have had their graphics and memory-model halves
  upstreamed already, so what is left of them is kernel-only and no longer needs
  splitting.
- **The netplay batch is the one place order is load-bearing.** The later fixes assume the
  earlier ones; sending them out of order produces PRs that do not make sense on their own.

### Remeasured 22 August, after #626-#640

`master` is at the merge of #640 and `ios-next` has merged it. The two earlier measures
above counted hundreds of files; **the code delta is now 34 files, +873/-87** (same
exclusions: `docs/` and `src/emu/qt/translations/`, the latter now at zero as well).
Everything the batches sent between 21 and 22 August covered — the whole services row,
the package row, the kernel row, common/vfs/utils, the scripting patches, the patch DLL
sources, the drivers row — is upstream. What still differs is best read as two buckets,
because the older tables mixed them and so overstated what is left to send.

**Bucket 1: fork infrastructure, which stays here by design.** None of this is a
candidate for upstreaming; upstream has its own iOS job in `build.yml` (#640) and does
not want a second one.

| What | Files | Lines |
|---|---|---|
| iOS CI workflows (TestFlight, unsigned IPA, dev-cert provisioning, signing maintenance) | 4 | +666/-0 |
| `scripts/` (the regression suite, the BIA gameplay test, the CPU smoke generator, the simulator seeder) | 4 | +1248/-0 |
| Repo meta (`AGENTS.md`, `CLAUDE.md`, `BUILDING.md`, `README.md`, `.gitignore`) | 5 | +154/-3 |
| `docs/` — this file and 120 root-cause write-ups | 121 | +12778/-0 |

**Bucket 2: emulator code still to send.** This is the real remainder.

| Area | Files | Lines | What it is |
|---|---|---|---|
| Netplay, Bluetooth, and the Bluetooth notifier | 18 | +631/-70 | the last batch of any size; internal order is load-bearing |
| Dispatch — the OpenVG swap heuristic | 5 | +75/-4 | full-surface image coverage tracking in `gnuVG`, and the deferred swap in `egl.cpp` it feeds |
| `kernel/svc.cpp` — start missing ROM daemons | 1 | +59/-0 | spawn `ClkNitzMdls.exe` when its start object is looked up, standing in for the boot sequence EKA2L1 never runs |
| `system/epoc.cpp` — iOS glue | 1 | +29/-2 | **sent as #641**; CPU-backend selection, `cache_root_`, the teardown flush, `runtime_resource_path` for the patch folder |
| `services/applist/applist.h` | 1 | +11/-8 | `delete_registry` moved to public for the iOS uninstall path |
| `src/tests/epoc` | 3 | +58/-0 | the SMS-PDU virtual-destructor static assert and the AknIconServer opcode pinning |
| `config/*.inl` | 2 | +2/-0 | `ios-use-jit` went with #641; the per-app `screen-mode` setting is still here |
| `common/src/upnp.cpp`, `ios/Bridge/IosEmulator.mm`, `qt/src/thread.cpp` | 3 | +4/-3 | a missing `platform.h` include, the teardown flush call, and one `pause_event.reset()` removed |

#641 took the `epoc.cpp` row, and it is worth saying what rebuilding it on upstream
turned up, because it is the same lesson as #607: all four hooks already had their
supporting half upstream and no consumer at all, so upstream today runs a JIT on
sideload builds with nothing asking for it, never frees a killed process's audio,
extracts mounted zips back into `Documents` after its own frontend cleaned that up,
and loads no patch DLL on iOS whatsoever. The fork's own version had the CPU log line
inside `#if EKA2L1_IOS_DYNARMIC`, which hid the backend from exactly the App Store
build whose logs matter most; the PR moves it out.

Three more rows are only nominally fork-specific and could go out as small PRs
tomorrow: the `upnp.cpp` include, the two test files, and the `applist.h` visibility
change. The `qt/src/thread.cpp` line is a one-line behaviour change to a frontend this
fork does not build, so it needs a desktop run before it is proposed. What genuinely
needs work is the netplay batch, and after that only the dispatch heuristic and the
startup-daemon hook remain as behaviour.

Four things that were rows in the 21 August table are now at zero: kernel objects and
lifetimes (down from 21 files/+862 to the one startup-daemon hook), services non-netplay
(down from 30 files/+1450 to one header visibility change), common/vfs/utils/config
(down from 25 files/+526 to two `.inl` lines and one include), and the patch DLLs.

**The merge itself was not free of judgement.** Four files conflicted, and in all four
the conflict was between the fork's original and upstream's rebuilt version of the same
change — #637's scripting split against the fork's, and #638's EKA1 IPC comment against
the fork's. Upstream's side won every time, because upstream's is the reviewed one:
`scripting/manager.{h,cpp}` are now byte-identical to upstream (the fork's
`eager_resolve` parameter had no caller passing anything but the default, and upstream
dropped it), the redundant `else() set (ENABLE_SCRIPTING 0)` went with it since
`#cmakedefine` treats an unset variable and `0` alike, and `svc.cpp` kept only upstream's
comment. Resolving these by checking out upstream's whole file is a trap worth naming:
`svc.cpp` still carries 59 fork-only lines, and taking upstream's copy of it silently
deletes them. Resolve the hunks, not the files.

### Remeasured 1 September, after #659-#662

`master` is at the merge of #662 and `ios-next` has merged it, cleanly and without a single
conflict. **The code delta is now 16 files, +712/-1** (same exclusions: `docs/` and
`src/emu/qt/translations/`). Four PRs took the whole of bucket 2 apart from two rows:

- **#659** — the EKA1 12-bit display mode. A follow-up to #658, which took the
  `dsa_disp_mode` measurement but left the older global `EColor4K` → `EColor64K` remap in
  `parse_wsini()`, so `master` briefly carried both. Removing the remap costs X-Plore on
  `nem-4`, which is stated in the PR rather than hidden: its zero-byte surface is a bug in
  its own 12-bit table, and paying for it by changing the pixel format of every application
  miscolours the games.
- **#660** — netplay, Bluetooth and the device-selection notifier, 18 files, sent as one
  commit because the fixes are sequential.
- **#661** — the per-app `screen-mode` serialisation, the two directory hot paths, and the
  two tests.
- **#662** — `flush_pending_teardown` in `closeRunningApp`.

**Three of the seven rows in the 22 August table were not work at all.** They are worth
naming, because the way they were miscounted is a measurement error, not an accident:

| Row | What it actually was |
|---|---|
| `common/src/upnp.cpp` — "a missing `platform.h` include" | A **dead** include, left by `4ec8f9ab4` when the file still had platform guards. The file has used no `EKA2L1_PLATFORM` macro since. Deleted here rather than sent. |
| `services/applist/applist.h` — "`delete_registry` moved to public" | Upstream declares it **public already**, in its original position, with the same doc comment, and the `IosEmulator.mm` callers are upstream too. What remained was a relocated declaration and a comment asserting "public here, unlike upstream" — an assertion that had stopped being true. Reverted. |
| The dispatch OpenVG heuristic (5 files) | The coverage bookkeeping and the `try_update` frame-boundary fix went upstream with #658's neighbours; what was left was the null `drawer` for VG contexts, plus another dead include (`<algorithm>`, orphaned when the coverage counting it served was deleted). The `drawer` choice is a *policy* change, not a bug fix — it leaves an OpenVG client with no frame limiting at all — so it was dropped rather than sent. |

The common thread: a `git diff` that reports a file differs tells you the file differs, not
that the difference is work. Each of these looked like a one-line portability fix from its
diff shape alone, and each dissolved the moment the question "who consumes this today?" was
asked against upstream's tree rather than the fork's. **Ask it before the row goes in the
table, not before the PR goes out.**

`config/app_settings.inl` is the counter-example that makes the check worth doing in both
directions. It looked like the same kind of one-line noise, and it is the opposite: upstream
holds `screen_mode` in `app_settings.h`, initialises it in `app_settings.cpp`, and both
applies and saves it in `screen.cpp` — only the `SETTING()` line that serialises it was
missing, so the value was read back as -1 on every launch and the whole path was dead. Same
shape as the four hooks #641 found.

**A verification failure worth recording.** #660's body first reported "87/95 passing, 8
pre-existing failures" for `ekatests`. There are no pre-existing failures. Its loader tests
open `loaderassets/` relative to the working directory; run from the repository root the
asset is missing, `simple_icon_handler` takes a SIGSEGV, and **Catch2 prints a partial
summary before the process dies** — which reads exactly like a stable set of known failures.
Run from the build directory the suite is fully green, 184 cases on `master` and 185 with
#661's added test.

The damage was not the wrong number. It was that the A/B built on it proved nothing: both
sides crashed at the same test, so both printed the same partial totals, and "identical to
master" was true and meaningless while 89 cases never ran. The tell is cheap —
`--list-tests` count versus the run's count, or exit code 139. **A harness that cannot fail
loudly has to be calibrated against a known-red mutation before its green is worth
anything**, which is the same lesson the revert-verify harness taught in August and which
did not transfer.

**What is left, and why neither row is a simple send:**

| Area | Files | Lines | The open question |
|---|---|---|---|
| qjpeg — host JPEG decode for Qt applications | 15 | +653 | The dispatch half (`eimage_decode_info` / `eimage_decode`, stb-backed) is platform-neutral and the prebuilt DLL matches what upstream already ships for fourteen other patches. The install path is the problem: `install_qt_image_plugins` lives in `IosEmulator.mm` and copies the DLL onto the C drive with a `.qtplugin` stub, which is not the shared `load_patch_libraries` route, so Android and Qt would get nothing. Move it to the shared layer first. |
| `kernel/svc.cpp` — start missing ROM daemons | 1 | +59 | Spawn `ClkNitzMdls.exe` when its start object is looked up, standing in for the boot sequence EKA2L1 never runs. The evidence is good — the Clock otherwise polls `ClkNitzMdlStartSemaphore` six times, a second apart, on every launch — but it is a table-driven behaviour hook, and upstream may want a general answer instead. Send it alone. |

Bucket 1 is unchanged and stays here by design: 4 iOS CI workflows (+580), 4 `scripts/`
(+1249), 5 repo-meta files (+171/-3), and `docs/` (139 files, +14633).

### Branches, remeasured with the same rule

Commit counts stopped being a valid unit of measurement for the tree, and they are no
worse a guide for branches: `ahead=N` says nothing about whether the content landed under
a different sha. Every branch below was judged by diffing its content against
`upstream/master`.

Absorbed, and deleted on 22 August (shas recorded so they can be resurrected):

- The eight `upstream/*` topic branches — `applist-sensor`, `chore`, `common`,
  `dyncom-breakpoint`, `eka1-ipc`, `esock`, `scripting`, `case-sensitive-volumes` — plus
  `codex/ios-upstream-release`. All are ancestors of `upstream/master`.
- `build/lunasvg-3.5` (`dc2ba7f38`). Upstream pins the same lunasvg commit
  (`51c65dc8`) and already carries `src/tests/epoc/services/svg_icon.cpp`; the branch's
  one commit is content-identical to what landed.

Absorbed since, and safe to delete (measured 1 September; `git diff upstream/master...<branch>`
is empty for each):

- `fix/camera-viewfinder-orientation` and `fix/eka1-n70-compat` (`358adacd2`), both merged
  upstream as #658 and its neighbours. GitHub pruned the first on merge.
- `codex/fix-ngage-games` (`0ae72327f`).
- The four PR branches for #659-#662, pruned on merge.

Kept, because their content is genuinely not upstream and not in `ios-next` either. Line
counts are `git diff ios-next...<branch> -- src`, which is the number that matters — several
of these look enormous against `upstream/master` only because they are stale:

| Branch | sha | vs `ios-next` | What is only here |
|---|---|---|---|
| `build/modernize-deps` | `d68811ac8` | 9 files, +49/-10 | macOS host build: the Apple Silicon ffmpeg@5 path, `#define stat64 stat` for SDK 26, the 11.0 deployment target that stops being forced over the caller's, `capstone-static` → `capstone`. The submodule bumps themselves landed. |
| `feat/guest-internet-access-point` | `0d653dde2` | 10 files, +494/-11 | CommsDat, central repository and esock — the retail-ROM "No Internet access point" fix. The only *feature* on this list. |
| `codex/ngage-wip` | `ad943a8a2` | 22 files, +1787/-248 | N-Gage work that the landed compatibility batch did not cover. |
| `codex/fix-ngage-ios` | `0e5e8a7f9` | 22 files, +1651/-110 | Overlaps `codex/ngage-wip`; the two should be reconciled before either is judged. |
| `integration/rhythmbelle-simulator` | `2687f2f63` | 17 files, +265/-69 | Title-specific integration work. |
| `fix/gles2-multi-string-shader-source` | `436a557e8` | 13 files, +166/-42 | Local only, never pushed. |
| `codex/ios-metal` | `f950f3bf9` | 9 files, +355/-47 | Experiments the fork has moved past — the MetalANGLE backend was removed in `249228878` and [the ANGLE plan](./ios_metal_angle_plan.md) supersedes it — but each still holds code that exists nowhere else, so deleting them loses that code rather than tidying it. |
| `ios-render-attemp` | `819d12f4f` | 14 files, +249/-20 | as above |

`ios` (`dce24a935`) is no longer the fork's default branch — `ios-next` is, and
`c0b62ae7e` cut TestFlight builds from it. `ios` is 190 commits behind `upstream/master`
and holds five commits `ios-next` does not, all TestFlight CI wiring, the last of which
made those builds manual-only. Nothing depends on it; it is kept only until that wiring is
either folded into `ios-next` or declared dead.

**A flag raised on 22 August is now wrong and is retracted:** upstream's `build.yml` does
*not* run an App Store signing path on a push to `master`. Its `build-ios` job is device
only and explicitly never signed, and says why in its own comment — signing on CI mints a
development certificate per run and Apple caps how many an account may hold. Pushing
`origin master` fast-forward fires an ordinary CI run and nothing else. Verified before the
1 September push.

## Sending the first behavioural batch

The memory model went out as #599 and merged the same day. It is the only behavioural
batch sent so far, so what it cost is the best estimate available for the ones after it.

**It was sent as one PR, not one per root cause.** Five defects, all on the subject of who
owns a chunk struct and when the CPU may still hold a translation into it. Rule 4 below
still stands as the default, but two of the five could not have been separated: releasing
a chunk slot on failure moves a destructor to the moment of failure, which changes when a
second, pre-existing defect fires. Bundling was the honest choice there, and saying so in
the PR body cost nothing.

**Porting was by rebuild, not cherry-pick.** Branch from upstream, apply each commit's
hunks, and check the result with `git diff ios-next` — the remainder should be only what
was deliberately left behind (here, one interpreter commit that happened to touch
`mem/src/mmu.cpp`). That check is what makes "the branch matches the fork's final state"
an assertion rather than a hope.

**Running the suite under ASan for the first time found a defect on upstream's tree.**
`multiple_mem_model_chunk` initialised none of its members, while its destructor opens
with `decommit(0, max_size_)` — so a chunk whose creation failed walked a page table array
with a garbage size. It reproduces on an unmodified tree, and it had been sitting behind
the fact that nothing ever ran these tests under a sanitiser. Two lessons:

- **A3 is not a nice-to-have.** One ASan run on an existing test suite produced a
  reproducible SEGV in shared code. That is the whole argument for the job, made in a
  single afternoon.
- **Reviewing your own batch before sending it pays.** The same pass found two dead
  branches in the fork's own fix — a loop guard defending against a case that cannot reach
  it, and a hand-off of a field (`flexible_mem_model_chunk::owner_`) that nothing in the
  tree ever reads.

**"Not testable without a ROM" deserves one attempt before it is asserted.** The batch's
lifetime fixes looked device-only and were described that way in the first draft of the PR.
They are not: two mem-model processes, one attaching to the other's global chunk, is enough
to reproduce the use-after-free — silently on an ordinary build, precisely under ASan. The
TLB half really does need a device, and that distinction is worth drawing per fix rather
than per batch.

## How the batches themselves should be sent

1. **Infrastructure first**, in order: A0 → A1 → A2/A3 → B1. Every later behavioural batch
   then lands behind a gate that already exists. These PRs also carry no behavioural risk,
   which makes them the cheapest way to establish trust. *A0, A1, A1b and A2 are done; A3
   is the next one, and nothing is blocked on it.*
2. **iOS-only changes and shared-emulator changes never share a PR.** A shared-code PR must
   be able to explain why the bug is also a bug on Qt and Android.
3. **A checklist per PR**: Track A green; Track B shadow results attached; the affected
   application plus one known-good control application shown working; the log scanned for
   panics, access violations, graphics halts and leftover diagnostics.
4. **Stay bisectable** — one root cause per PR, no bundling. The per-bug documents in
   `docs/` are ready-made PR descriptions and materially reduce review cost.
5. **Open an issue before the PR** for anything that touches the maintainer's own ground —
   A0 (the workflow) and B1 (a new target) in particular.

## Deliberately out of scope

Worth doing eventually, wrong thing to attach to this plan:

- **Compatibility telemetry** in the style of report.ppsspp.org, with a per-title flag file
  like `compat.ini`. EKA2L1 has neither the per-application flag infrastructure nor a
  server, and it raises privacy questions that deserve their own discussion.
- **Frame-dump replay.** EKA2L1 has no GPU command-stream dump format, so this is a
  subsystem to design, not a checklist item — even though it is the most valuable long-term
  addition of the three.
- **Savestate versioning tests**, which should wait until the state format settles.

## Sequencing

| Step | Track | Status | Can be PR'd as-is | Needs maintainer buy-in |
|---|---|---|---|---|
| A0 workflow repair | A | **merged** (#583) | — | discussed first (their CI) |
| A1 `ekatests` repair + `ctest` in CI | A | **merged** (#584) | — | no |
| A1b toolchain modernisation (unplanned) | A | **merged** (#586, #585) | — | no |
| A2 `dyncom_difftest` in CI | A | **merged** (#604, with the interpreter batch) | — | no |
| A3 ASan/UBSan job | A | **sent as #609** (found four, all on master) | yes | no |
| A4 regression tests alongside each batch | A | **in force** since #589 | yes | no |
| B1 headless frontend | B | not started | yes | discuss first (new target) |
| B2 intests driver + committed SIS | B | not started | yes | no |
| B3 screenshot/MSE harness | B | not started | yes | no |
| B4 self-hosted runner | B | not started | no | required |
| B4′ shadow CI on this fork | B | not started | n/a | none |

Outside the plan, the port itself went out and was accepted: #587 (the iOS frontend), #592
and #596 (its CI job), plus #588, which gave upstream one stable required check for branch
protection. Behavioural batches landed so far: #599 (memory model), #600 (the central
repository INI reader), #601 (the domain manager), #602 and #603 (audio and video), and
#604 and #605 (the interpreter, the first of which carried A2 with it), and #606
(the window server, the font store and the screen driver). The 31 August round closed
bucket 2 down to two rows: #659 (the EKA1 12-bit display mode), #660 (netplay, Bluetooth
and the device-selection notifier), #661 (a lost per-app setting, two directory hot paths
and two tests) and #662 (the deferred teardown queue on iOS app close).
