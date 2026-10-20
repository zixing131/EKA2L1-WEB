# Audio fixtures

`tone.amr` is a generated 440 Hz sine wave: 8 kHz mono, 20 frames
(0.4 seconds), encoded using OpenCORE AMR-NB 0.1.6's MR122 mode with VAD
disabled. It contains the six-byte `#!AMR\n` header and twenty 32-byte
storage frames. It is test signal data, not audio extracted from a game.

The decoder follows the single-channel storage format in
[RFC 4867 §5.3](https://www.rfc-editor.org/rfc/rfc4867#section-5.3).
