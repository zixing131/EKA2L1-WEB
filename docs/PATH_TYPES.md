# Guest and host filesystem paths

Symbian file names must retain their original spelling even when lookup ignores
case. Lowercasing names during installation hides lookup bugs and changes names
returned to guest applications. Conversely, applying that policy to Linux host
files can overwrite distinct files. The two domains therefore use separate value
types throughout the emulator's owned filesystem APIs and state.

`guest_path` stores the original UTF-16 spelling and a cached lookup key. Equality,
ordering, hashing and searches all use the same key: slash aliases are unified and
characters use Unicode 5.0 simple case folding (C/S mappings from Symbian's Unicode
source data). The process locale does not affect identity. Mutation reconstructs
both values together; callers cannot mutate the stored string directly.

This key is lexical case-insensitive identity, not descriptor collation or a
filesystem object identifier. It does not collapse `..`, resolve mounts, strip
accents or apply Unicode normalization. Symbian descriptor `CompareF` and
individual ROM/ROFS filesystem implementations have additional or narrower rules;
they are not interchangeable with general path identity. In particular, `café`
and `cafe` remain distinct names.

`host_path` stores UTF-8 spelling and uses exact lexical equality, ordering and
hashing. Native I/O retains the host filesystem's own rules. Android content URIs
also belong to this domain and use the existing document-provider adapter.
Neither type converts implicitly to a string or to the other path type.

## Crossing domains

VFS methods accept guest paths. Mounts and `get_raw_path` results are host paths.
Mapping a relative guest name below a host root uses `root.resolve_guest(name)`;
it locates existing components by guest identity and retains the spelling of
components being created. A drive-qualified guest name must first be mapped
through its mounted drive. Passing its text directly to a host constructor is
not a mapping.

Native `copy_folder` keeps host names distinct. `copy_guest_tree` explicitly merges
case aliases when copying an emulated filesystem tree. Installers preserve names
from the source image and resolve existing guest destination components instead
of lowercasing them.

The directory lookup cache uses native subsecond modification timestamps and is
invalidated by owned namespace mutations, including file creation through
`open_c_file`. Android document-provider lookups bypass the native timestamp cache.

Encoding is explicit at boundaries: `.utf8()` or `.utf16()` for JSON, YAML,
descriptors, C APIs, Qt, JNI and Foundation; explicit constructors or `from_utf8`
and `from_utf16` on entry. Configuration and saved-state text formats retain their
existing representation. Embedded NUL is rejected; IPC path readers reject it
before constructing a path.

Wire-format records, archive entry identifiers, unvalidated filename syntax and
descriptor parsing buffers remain text until interpreted. URLs, HTTP route
segments, command lines, module identifiers and SVG geometry are not filesystem
paths. The raw helpers in `common/path.h` support these parser and native-adapter
boundaries; overloads on typed values preserve the selected domain.

## Contract references

The source review used SymbianSource's `oss.FCL.sf.os.kernelhwsrv` at
`0c3208650587ac0230aed8a74e9bddb5288023eb`: ROM lookup in
`userlibandfileserver/fileserver/srom/sr_rom.cpp`, ROFS comparison in
`userlibandfileserver/fileserver/srofs/dircache.cpp`, and simple folding data in
`kernel/eka/euser/unicode/perl/CaseFolding-5_0_0.txt`. The imported Unicode table
has its accompanying license beside `common/src/path_case_folding.inl`.
