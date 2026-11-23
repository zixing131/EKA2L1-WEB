# GDI command payloads need their own alignment

The sanitizer job for upstream PR #678 stopped in the new pending-texture-upload
test. UBSan reported an eight-byte-aligned texture-update payload being referenced
at an address ending in `534`, inside `gdi_store_command::get_data_struct`.

The upload test exposed an existing storage defect. The command places a byte array
immediately after its four-byte opcode and casts that array to payload structures.
The enclosing command contains a shared pointer and is itself suitably aligned, but
that does not align the array member at offset four. Several payloads contain host
pointers, handles or `size_t`, so the problem affects more than texture updates.
Normal builds passing on macOS and Linux did not establish that these references
were valid. A standalone ASan/UBSan program using the production accessor reproduced
the same error without a graphics driver or guest process.

The byte array now explicitly uses `alignas(std::max_align_t)`. Both accessors reject
payloads exceeding the buffer capacity or its alignment at compile time. Tests cover
all eight payload types used by the command builder, including consecutive command
objects and const/mutable access. The command is host-only transient storage; no
Symbian IPC layout, serialized format or guest patch DLL changes are involved.
