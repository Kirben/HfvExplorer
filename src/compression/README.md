These are codec sources only; the HFS+/HFSX filesystem implementation remains
HFVExplorer's own code. All codecs are linked statically, without a new DLL
or Windows API requirement. The zlib deflate encoder is also built for decmpfs creation; gzip/file APIs are excluded.

- `zlib/`: the inflate/deflate subset of zlib 1.4, upstream commit
  `99623ef7097352da7993ba614161fd6a311e046e`. Source: https://github.com/madler/zlib/tree/v1.4.
  Built with Z_SOLO and Z_PREFIX; the caller supplies bounded allocation.
  License: zlib/LICENSE and ../../licenses/zlib.txt.
- `lzfse/`: unmodified Apple LZFSE and LZVN encoder/decoder sources, commit
  `e634ca58b4821d9f3d560cdc6df5dec02ffc93fd`.
  Source: https://github.com/lzfse/lzfse.
  License: lzfse/LICENSE and ../../licenses/LZFSE-BSD.txt.

- `libzbitmap/`: MIT-licensed Corellium LZBITMAP encoder/decoder, upstream commit
  `d12d6c346daf41f770f6516bcb14f0a8c7441e31`.
  Source: https://github.com/eafer/libzbitmap.
  License: libzbitmap/LICENSE and ../../licenses/LZBITMAP-MIT.txt.
  Local changes: bitmap repetition stops when the declared chunk output is complete. Our
  decmpfs wrapper additionally checks all chunk lengths, total output, the
  terminator and consumed input before invoking the decoder.

Compressed-file output is limited to the declared logical length and one
64 KiB compression unit at a time. Exact output length and decoder status are checked. Native type-4 zlib units
may finish at a sync-flush boundary; resource-based LZVN units may contain
padding after their end-of-stream opcode. Inline streams require completion
and exhausted input. Unsupported decmpfs methods are reported explicitly.

HFVExplorer independently constructs all supported decmpfs output types:
legacy stored (1), zlib (3/4), LZVN (7/8), stored (9/10), LZFSE (11/12)
and LZBITMAP (13/14). Inline attributes respect the 3802-byte header-inclusive
limit. Legacy type 1 is inline only; larger stored files use type 9/10.
Resource output uses independent 64 KiB units. Type 4 has mixed-endian block
tables and a single unnamed cmpf resource map; other resource methods have a
little-endian offset table with a terminal offset. Incompressible units use
method-specific stored markers. A planning pass bounds 32-bit resource offsets;
a second pass checks each encoded unit's length and checksum before publication.
Overwrites preserve the codec family, promoting inline data to resource storage
when needed. Explicit conversions retain user metadata and refuse to consume a
genuine resource fork. No APSL implementation was incorporated. Additional
unmodified LZFSE/LZVN encoder files use the same pinned BSD-licensed revision;
the existing MIT libzbitmap encoder is now built as well as its decoder.
