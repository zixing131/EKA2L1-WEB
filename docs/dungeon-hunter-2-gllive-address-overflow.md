# Dungeon Hunter 2: menu music hiss and KERN-EXEC 3 on "Single Player"

## Symptom

On the X7 (rm-707), Dungeon Hunter 2 (UID `0x2003B2CE`) showed two problems that looked
unrelated:

* From the moment the title screen music starts, a continuous broadband hiss sits on top
  of it.
* `Start game -> Single Player` kills the process with `KERN-EXEC 3` roughly 6 times out
  of 10. The rest of the time the level loads normally.

Both turned out to be the same single byte written in the wrong place.

## Narrowing it down

### The audio is already wrong when it leaves the guest

The emulator's own audio path was exonerated first, which saved a lot of time:

* Dumping the PCM the guest hands `CMdaAudioOutputStream::WriteL` and the PCM the
  AudioUnit render callback receives, then walking both streams in lockstep, showed
  **zero** differing samples over 2.7M samples — the only difference was zero padding at
  the few underruns. The host side is bit-exact.
* Every read of `sounds.zip` was traced (position/length/result): all reads fully
  satisfied, none short.
* dyncom and dynarmic produced statistically identical audio, so it is not a CPU
  emulation bug.

Comparing against a reference decode of `m_title.vxn` (IMA ADPCM 32 kHz stereo, decoded
with ffmpeg and resampled to the 16 kHz the game streams at) made the damage measurable:

```
                spectral flatness   energy > 4 kHz
original             0.005              0.000
guest output         0.35               0.28
```

The noise had structure: a burst of ~40 samples that slams to the rail, repeating with a
period of exactly 1017 output samples — one IMA ADPCM block. The phase stayed locked over
tens of seconds, so it was data-driven, not a race.

### The crash is guest heap corruption

A temporary probe in `kernel_system::cpu_exception_handler` dumped the guest registers,
a raw stack window and a codeseg-resolved backtrace at the access violation:

```
pc = euser.dll + 0x1A8B6   (dlmalloc's dlfree, backward-coalesce arm)
lr = euser.dll + 0x2ABC7   (RHeap::Free)
  <- libc.dll + 0x993D     (free)
  <- dungeonhunter2.exe    (Open C / pthread app)
```

`dlfree` had found `PINUSE` clear on the chunk it was freeing and followed `prev_foot`,
which was PCM data, into unmapped memory. The freed pointer is recoverable from the
faulting frame as `r1 + r3` (`p - prev_foot` plus `prev_foot`) and came out as the same
address, `0x05DB6500`, in every crashing run.

Note the trap here: the obvious-looking pointer on the stack (`0x05DB5DE8`) is a
different local. Deriving `p` from the registers is what pointed at the right chunk.

### Catching the writer

The decisive tool was a guest write watchpoint added to dyncom's inline
`WriteMemory8/16/32/64` **and** to `WriteMemory32Block` (the LDM/STM cursor — a probe that
misses it sees nothing for block transfers), driven by env vars for address range, value,
PC/LR filter and a hit cap.

Watching the corrupted chunk header showed 1798 writes, all from euser's `memmove` called
from the game's audio mixer: that region is the game's audio staging buffer, so
`0x05DB6500` was never a `malloc` result at all — the pointer handed to `free()` was
already wrong.

Chasing the *value* `0x05DB6500` found nothing, because it is never written as a 32-bit
word. Switching the probe to "report when the aligned word covering the store *becomes*
the watched value" caught it immediately:

```
WPPROBE oldword @0x010D3900 = 0x05DB6578
WPPROBE write   addr=0x010D3900 size=1 data=0 pc=0x703335A0 r4=0x010D38F4 r3=0x0000000C
```

`0x05DB6578 & ~0xFF == 0x05DB6500`. A single zero byte had cleared the low byte of a live
pointer.

### The instruction that does it

Dumping the guest's code segments at the fault (`EKA2L1_DUMP_CODESEG`) and disassembling
`dungeonhunter2.exe` around that PC gives:

```asm
bl   insock!TInetAddr::TInetAddr        ; local TInetAddr
bl   esock!RSocket::RemoteName          ; peer address of the connected socket
bl   euser!TBufBase16::TBufBase16(50)
bl   insock!TInetAddr::Output           ; "198.18.15.95"
bl   euser!TBufBase8::TBufBase8(128)
bl   euser!TDes8::Copy(const TDesC16&)
bl   euser!TDes8::PtrZ
ldr  r4, [fp, #-0x130]                  ; the TBuf8 iLength word
bic  r0, r4, #0xf0000000                ; Length()  -> 12
bl   operator new                       ; new char[12]
bl   euser!memmove                      ; copy 12 bytes
bic  r3, r0, #0xf0000000
strb ip, [r4, r3]                       ; buf[12] = 0   <-- one past the end
```

The game strdups the peer address text but allocates `Length()` instead of
`Length() + 1`. `operator new` in `stdnew.dll` allocates exactly what it is asked for, and
RHybridHeap's slab hands out 4-byte-granular cells with no per-cell header, so a 12-byte
request gets a cell that is exactly full. Byte 12 lands on the first word of the next
cell — which, at that instant, is the audio engine's 3-entry buffer-pointer table.

That single byte explains both symptoms:

* the mixer then reads and writes its buffer 0x78 bytes off, producing the saturated
  burst once per decoded block — the hiss;
* the audio teardown later calls `free()` on the mangled pointer — the `KERN-EXEC 3`.

### Why it only bites sometimes

The overflow is harmless unless the string exactly fills its slab cell, i.e. unless the
dotted-quad text length is a multiple of 4. IPv4 text is 7..15 characters, so only 8 and
12 are fatal.

`gllive.gameloft.com` still resolves publicly to `208.71.185.242` — 14 characters, safe.
The machine that hit this runs a fake-IP proxy (Surge/Clash style) that answers every
name from `198.18.0.0/15`, and it handed out `198.18.15.95` — 12 characters. The proxy
also accepts the TCP connection locally, so the connect always succeeds and the
"connection established" handler always runs.

Verified by substituting the socket's reported peer address (connect still used the real
one):

| peer address reported to the guest | result |
| --- | --- |
| `198.18.15.95` (12 chars) | crash 2/2, hiss present |
| `10.0.2.100` (10 chars) | survives 2/2 |
| `208.71.185.242` (14 chars, the real one) | survives 2/2, audio flatness 0.0059 (clean) |

Faking only `LocalName` changes nothing, which is what pins the formatted address on
`RemoteName`.

Dead ends worth skipping next time:

* The audio hiss looks like a decoder or resampler problem. It is not; the guest's decode
  is fine until the buffer pointer moves under it.
* Disabling the audio stream to A/B the crash is not conclusive — the game then fails
  differently (a null dereference in `drtaeabi.dll` from the audio construction leave).
* The exclusive monitor / RFastLock angle for "flaky heap corruption in a pthread app" is
  a red herring here; the corruption is a plain one-byte overflow.

## Root cause

A one-byte heap overflow in Dungeon Hunter 2's Gameloft LIVE code. It fires whenever the
game reaches a Gameloft LIVE server and the peer address formats to 8 or 12 characters.
Nothing on the emulator side can make the guest's overflow harmless while the connection
succeeds, because the address the game formats is the true peer address.

The practical fix for a machine behind a fake-IP proxy is to keep `gllive.gameloft.com`
out of fake-IP mode (route it DIRECT, or REJECT it). Either the real 14-character address
is used, or the connection fails the way it does on a phone with no route — both are safe.

## Emulator defects found on the way, and fixed

Neither of these causes the crash, but both are real and were proven wrong against the
Symbian contract while chasing it.

### IPv4 addresses were handed to the guest byte-reversed

`TInetAddr` stores the IPv4 address as a **host-order** `TUint32` — that is what
`TInetAddr::Address()` returns and what `TInetAddr::Output()` formats from.
`host_sockaddr_v4_to_guest_saddress` copied `sin_addr` verbatim, leaving it in network
order, and `GUEST_TO_BSD_ADDR` copied it back the same way. The two cancelled out, so
connecting worked, but every address the guest *read* came out with its octets reversed
(`198.18.15.95` was shown to the game as `95.15.18.198`), and any address a guest builds
itself with `INET_ADDR(a,b,c,d)` was sent to the host reversed.

Both conversions now use `ntohl`/`htonl`. The places that were written against the old
convention were updated with them: the loopback probe in `retrieve_local_ip_info`, the
Windows netmask from `ConvertLengthToIpv4Mask`, the LAN-discovery sender comparison, and
the netplay matching-server wire format (which stays network-ordered on the wire, so
existing peers keep working).

Verified: the guest now sees `127.0.0.1`, `255.0.0.0`, `192.168.1.11`, `255.255.255.0`,
`192.168.1.255` and `198.18.15.95` instead of their reversals. `ekatests` pins both
directions against `INET_ADDR` / `KInetAddrLoop` as `in_sock.h` defines them.

### Stereo audio position and bytes-rendered were doubled

`dsp_stream::samples_played_` counts interleaved samples (frames x channels), but
`dsp_output_stream_shared::position()` divided by the frequency alone, and
`bytes_rendered()` multiplied by the channel count a second time. On a stereo stream
`CMdaAudioOutputStream::Position()` therefore ran at twice real time and the rendered byte
count was twice the truth. Dungeon Hunter 2 never calls either (checked with a counter),
but a title that paces its writes off `Position()` would be driven off the rails. Both now
divide out the channel count, and `ekatests` pins mono and stereo against the microsecond
figure `CMdaAudioOutputStream::Position()` is documented to return.
