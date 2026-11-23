# RPKG extraction repeatedly statted the same directory hierarchy

Importing the X7 ROM and its 177 MB RPKG felt substantially slower on iOS than
on Android. The input was already inside the app's local Documents directory,
so neither a security-scoped provider nor an iCloud download was involved.

Profiling a Release simulator build showed that RPKG extraction was dominated
by filesystem metadata rather than payload I/O. X7 expands to roughly 7,400
files. Before opening every output file, `extract_file()` called
`create_directories()` for its parent. That function walked the complete host
path and called `get_file_type()` (and therefore `stat()`) for every component,
even when the same parent had already been used by many earlier entries.

On a warm duplicate-device import, the installation worker accumulated 3,221
one-millisecond samples. Directory creation accounted for 1,053 samples, with
999 of those stopped in `stat()`. Deleting the temporary tree after the
duplicate-device check was a separate 1,172-sample cost and should not be
mistaken for extraction time.

The fix has two scopes. The common `create_directories()` helper now returns
immediately when the complete target is already a directory, avoiding a full
component walk for any idempotent caller. RPKG extraction additionally keeps a
set of parent directories proven usable during that one import. Repeated files
in the same directory skip directory creation altogether. The cache is local to
the extraction, so deleting or replacing directories later cannot leave stale
process-wide state. A directory is cached only after its first output file opens
successfully, preserving retries after a creation failure.

With the same X7 package and duplicate-device rollback, the worker fell from
3,221 to 2,616 samples. Top-of-stack `stat()` samples fell from 1,187 to 327.
After subtracting the essentially unchanged rollback, the extraction portion
was about 30% faster. Samples inside directory creation fell from 1,053 to 165,
and its `stat()` samples fell from 999 to 126 (about 87%). The remaining
duplicate rollback cost is independent of this fix.
