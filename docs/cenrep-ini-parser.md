# A repository setting spelled `""` took the emulator down

## Symptom

A TestFlight `EXC_BAD_ACCESS (SIGSEGV)` from 26.7.0 (260878), reproducible on demand: open a
particular game on a 5320 (rm-409) and the app dies immediately.

```
Exception Subtype: KERN_INVALID_ADDRESS at 0x0000000000000027
esr: 0x92000006 (Data Abort) byte read Translation fault

0  eka2l1::parse_new_centrep_ini(std::string const&, eka2l1::central_repo&) + 1952
1  eka2l1::central_repo_server::load_repo_adv(...) + 3488
2  eka2l1::central_repo_server::load_repo(...) + 132
3  eka2l1::central_repo_client_session::init(eka2l1::service::ipc_context*) + 104
4  eka2l1::service::server::process_accepted_msg() + 968
5  eka2l1::epoc::session_send_general(...)
```

## Narrowing down

No dSYM was needed. The fault address and the register dump name the bug between them:

- `0x27` is 39, and the faulting access is a **byte** read. `ini_value` lays out as vptr (8) +
  `node_type` (4, padded to 16) + `std::string value` at offset 16. libc++ keeps a short
  string's size and its long/short flag in the *last* byte of the 24-byte string object, so
  copying a `std::string` living at offset 16 of a null object reads byte 16 + 23 = 39. The
  crash is `nullptr->get_value()`, not a corrupted pointer.
- `x6 = 0x3031303041303030` is `"000A0010"` in memory order — the key the loop was on, minus
  its `0x` prefix, sitting inline in a short `std::string`.

`ini_pair::get<T>(idx)` returns `nullptr` when the pair has fewer values than asked for, and
`parse_new_centrep_ini` dereferenced it unchecked. So the setting for key `0x000A0010` parsed
with **one** value where the parser wanted two. In the 5320 ROM, `1000102c.txt` (SMS service
settings, the repository a messaging-capable app opens on startup) ends with:

```
0x000A0010  string  ""
```

The generic INI tokenizer in `common/ini.cpp` reads a quoted token, gets an empty string back,
and `peek_string()` treats *any* empty token as "nothing left on the line" — so the value was
dropped entirely and the pair kept only its type token.

That same drop is worse than a crash when the empty value is not last: `0x1 string "" 0`
silently stored `"0"`, the metadata column, as the setting's value. Every ROM here ships
repositories with `""` values (71 files on rm-409, 135 on rm-707), so this was not an exotic
input.

## Fix

Two layers, because the parser had no business crashing on ROM content in the first place.

**The tokenizer** (`common/ini.cpp`) now carries a `quoted` flag with each token. An empty
token that was quoted is a real, empty value and stays in the stream; only an unquoted empty
token still means end of line. This keeps every other INI consumer honest too.

**The central repository parser** was rewritten to follow Symbian's own reader rather than the
generic INI grammar. `CIniFileIn` (`persistentstorage/centralrepository/common/src/inifile.cpp`)
walks the file as a single `TLex` token stream with a fixed section order — `cenrep`, `version`,
optional `protected`, `[owner]`, `[timestamp]`, `[defaultmeta]`, `[platsec]`, `[main]` — and
several parts of the format only make sense in that reader:

- strings may be quoted with `'` or `"` **or** unquoted up to the next blank, and expand
  backslash escapes (`\n`, `\"`, ...) in both cases;
- `binary -` is how the format spells empty data, not a byte;
- `int` accepts a negative decimal as well as `0x` hex;
- everything after the value is optional: a metadata column, then per-setting access policies
  (`cap_rd=...`), which a positional parser happily mistook for values;
- `string` is stored as raw UCS-2 bytes while `string8` is UTF-8 — the same distinction the
  guest reads back.

Deliberate divergences from upstream, all in the direction of not losing a whole repository:
a version above 1 is recorded instead of rejected, a malformed *setting* is logged and skipped
instead of failing the load, `[main]` is searched for if the sections come out of order, and
the `[platsec]` policies are still only skipped (`TODO (pent0): Capability supply` stands).

Two bugs fell out of the rewrite:

- The mask form of a `[defaultmeta]` range is `<lowKey> mask = <mask> <meta>`; the old parser
  read the first number as the metadata and the *third* as the low key, i.e. it swapped them.
  `get_default_meta_for_new_key` also matched masks as `(mask & key) == (low & key)` where
  `TSettingsDefaultMeta::IsInRange` is `(mask & key) == low`. Both now match upstream.
- `ini_loader_no_default_meta_range` asserted `intd == 15` on a `real` entry. It passed only
  because the old loop reused one uninitialised `central_repo_entry` across iterations, so the
  previous setting's integer was still in the variant. The value belongs to the entry before
  it; the test now says so.

## Verification

- The three `creiniloader` cases plus a new `ini_loader_all_value_types` over an asset that
  exercises every type, escapes, `-`, both `mask =` and `mask=`, trailing policies and the
  metadata defaults. Full suite: 104 cases, 430 assertions.
- Every repository INI in the rm-409 and rm-707 ROMs parsed through the new reader in-process:
  284/284 and 469/469 (the only rejects are `.cre` and `.xml` files, which are not INIs).
  `1000102c.txt` yields its 64 settings with `0x000A0010` an empty string.
- `scripts/ios_regression_test.sh` against a Release simulator build: 12/12.

One trap worth remembering: the first attempt read the file with
`dynamic_ifile::getline(std::u16string&)` unconditionally, which converts 8-bit files from
UTF-8 and **throws** on a stray byte — an exception escaping into the emulator instead of a
parse failure. 8-bit repositories are now widened byte by byte, and keyword matching never
converts at all.
