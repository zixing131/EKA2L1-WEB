# ROM stub registration spent minutes resolving wildcard paths on iOS

After importing an S60v3 ROM on a physical iPhone, the home screen could keep
showing its busy spinner without ever publishing the application list. UIKit was
still responsive and there was no guest panic or host crash. Different ROMs
appeared to stop at different packages: the N95 8GB China dump ended at Web
Browser, while a 5320 FP2 dump ended at VisualRadio.

The emulator log initially made the last package look suspicious. Web Browser
does describe more than two thousand ROM files, but changing ROMs disproved a
package-specific parser failure: VisualRadio reproduced the wait with only a few
dozen descriptions. Repeated log captures stayed byte-for-byte identical after
the package registration line, which narrowed the wait to the unlogged part of
the SIS interpreter rather than ROM extraction or application-list scanning.

A Time Profiler capture from the physical device showed the boot worker running,
not waiting on a lock. Every sample was below
`physical_file_system::get_real_physical_path`, in
`resolve_case_insensitive_path` and `find_case_sensitive_file_name`, with
`opendir`, `readdir`, and string comparison at the top. The simulator did not
reproduce the cost because its host volume matches filenames without regard to
case; the iOS app data container does not.

The regression came from replacing the iOS-only case resolver with the shared
case-sensitive-host implementation. That was correct for concrete mixed-case
paths, but ROM stub controllers also contain wildcard descriptions such as
`VisualRadio.r*` and `Clockapp.r*`. A wildcard cannot satisfy a literal
`exists()` check, so the fallback enumerated the complete containing directory
trying to find a case-insensitive literal match. The shared helper also requested
detailed directory entries unconditionally, turning each name scan into a
`stat` for every entry. Large ROM directories multiplied that work enough to
look like a permanent boot hang.

There were two distinct contract mistakes. A wildcard component is a pattern for
a later directory operation and must not be resolved as a concrete host name.
Also, a ROM stub has no data units to extract, so its install block does not need
host output paths at all; its virtual targets are sufficient for package
registration. The fix stops case recovery at the first wildcard, requests
detailed directory entries only when filtering by entry type, and moves SIS host
path resolution inside the branch that actually schedules data extraction.

The case-path unit test uses a literal mixed-case filename containing `*` as a
POSIX sentinel. The old resolver incorrectly finds that entry; the corrected
resolver preserves the already-resolved directory prefix and leaves the wildcard
pattern untouched. The existing package-manager suite continues to cover normal
SIS extraction, conditional files, upgrades, embedded packages, and uninstall
ownership, ensuring that removing the stub-only work did not remove real install
work.
