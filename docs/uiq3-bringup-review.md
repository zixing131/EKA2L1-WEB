# UIQ 3 bring-up contract review

The branch starts from `06a9943ba`. Its P1i R6E30 World1 firmware identifies itself as Symbian 9.1, machine UID `0x20002E69`. The ROM, ROFS and ROFx layers are vendor-specific, while most of the loader, graphics and IPC behavior is shared with other Symbian devices. Device-specific decisions belong at firmware identification, boot selection, HAL geometry or input translation. The individual services should apply their IPC contracts wherever the same ABI is present.

## Kernel and boot

The public executive sources list the two UserSvr TLS signatures at different EUSER ordinals. The 9.1 compatibility check now looks for the UID-aware export in ROM EUSER. A failed lookup does not cache an ABI choice. Export relocation for patched RAM images stays per ordinal because a single table may contain linked offsets and already relocated patch addresses. The dynarmic pin supplies the impossible TLB tag on its x64 lookup path. The iOS arm64 check does not exercise that backend.

The moving memory model uses a ROM executable's linked data/BSS address for fixed image data. In the P1i log that is `0x65f00000` for EComServer. Returning the existing process on a duplicate creation would grant the caller a handle to that process and make subsequent Resume and Rendezvous act on the wrong instance. The loader now reports a collision, including for fixed images with BSS only. The emulator still maps this address through its shared chunk implementation. The official moving-model process uses a process-owned fixed chunk.

UIQ startup processes remain selected by the ROM platform. Failed host-side scheduling clears the rendezvous barrier. Successful scheduling still relies on the ROM server rendezvousing or exiting, so another ROM variant with a differing startup protocol can stall the queue. P1i boot reaches its apps with the tested sequence. Its startup movie separately reports a missing `mpeg4decdrv` followed by `E32USER-CBase 45`. [Symbian kernel, executive and moving memory model](https://github.com/SymbianSource/oss.FCL.sf.os.kernelhwsrv/tree/master/kernel/eka)

## Display and firmware

P1i's `wsini.ini` declares five slots and explicitly leaves slot 0 empty, with slot 1 as the startup mode. The non-NGA reference preserves sparse indices, starts at the first supported mode, and enumerates supported entries only. The emulator now rejects attempts to select a hole at the screen boundary and in the guest app-mode operation. iOS lists only supported modes. Qt and Android still present a positional list, so they can display a placeholder for a hole. The framebuffer minimum of 256 pixels per row is scoped to the verified P1i machine UID. Broader Symbian 9.1 direct-framebuffer polling remains an emulator compatibility choice.

The SCDV inverse tables match all 512 and 4,096 entries in Symbian colourpalette. The built general DLL retains the frozen 31-slot export layout with its GDI imports removed. The EKA1 binary retains its seven slots. FBServ's modern resize client supplies an output package in slot 3. The branch now uses descriptor capacity to distinguish that case from the early caller that retains its original handle.

The converter honors the official ROFS hidden-file marker, directory entry boundaries and starting offset. The converted P1i ROM and RPKG are byte-for-byte identical to the previously backed-up files. Other vendor layouts and firmware families remain outside the tested set. [Symbian WSERV, FBServ and GDI](https://github.com/SymbianSource/oss.FCL.sf.os.graphics) · [Symbian ROFS format](https://github.com/SymbianSource/oss.FCL.sf.os.kernelhwsrv/tree/master/userlibandfileserver/fileserver)

## Client/server contracts

- Central Repository now stages Create and Set in the subsession, exposes staged values to that client's Get and Find, and discards changes on Cancel. Commit counts distinct changed keys and persists before notifying. Later and 9.1 Find use different descriptor positions. The implementation still covers the existing command set. Full repository backup/restore, delete/move transaction operations, client panic behavior and platform security policy enforcement remain incomplete.
- ViewServer reads the policy in slot 0 and writes the result in slot 2. It now evaluates the security identity captured with the delivered activation event and returns `KErrUnknown` outside that event. AppArc's indexed icon request uses the documented 24, 32 and 48 pixel sizes with the existing closest-size selection.
- ESock uses the connection registry to emit each interface up/down transition once, with its connection info and state in the caller's slot 0 buffer. Subscription cancellation and destruction release the pending request and queued events. This covers emulated connections.
- DevSound keeps a reference to the event queue obtained from the sending thread. Invalid handles and unsupported states complete before the queue is replaced. ChangeNotifier's `0x100` bit is thrash level. Its initial mask excludes the separate `0x80` low-memory event.
- WSERV's one-group-per-client delivery is per screen. The emulator limits no-op success to the setup calls observed on this ROM and returns an error for other unknown client operations. Full hotkey and fade configuration behavior remains unimplemented.

See [Central Repository client and server](https://github.com/SymbianSource/oss.FCL.sf.os.persistentdata/tree/master/persistentstorage/centralrepository), [AppArc and ViewServer](https://github.com/SymbianSource/oss.FCL.sf.mw.appsupport/tree/master/appfw) and [ESock](https://github.com/SymbianSource/oss.FCL.sf.os.commsfw/tree/master/datacommsserver/esockserver).
