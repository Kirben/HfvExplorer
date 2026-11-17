These are codec sources only; the HFS+/HFSX filesystem implementation remains
HFVExplorer's own code. All codecs are linked statically, without a new DLL
or Windows API requirement. The zlib deflate encoder is also built for decmpfs creation; gzip/file APIs are excluded.

- `zlib/`: the inflate/deflate subset of zlib 1.4, upstream commit
  `99623ef7097352da7993ba614161fd6a311e046e`. Source: https://github.com/madler/zlib/tree/v1.4.
  Built with Z_SOLO and Z_PREFIX; the caller supplies bounded allocation.
  License: zlib/LICENSE and ../../licenses/zlib.txt.
- `lzfse/`: unmodified Apple LZFSE and LZVN decoder sources, commit
  `e634ca58b4821d9f3d560cdc6df5dec02ffc93fd`.
  Source: https://github.com/lzfse/lzfse.
  License: lzfse/LICENSE and ../../licenses/LZFSE-BSD.txt.

- `libzbitmap/`: MIT-licensed Corellium LZBITMAP decoder, upstream commit
  `d12d6c346daf41f770f6516bcb14f0a8c7441e31`.
  Source: https://github.com/eafer/libzbitmap.
  License: libzbitmap/LICENSE and ../../licenses/LZBITMAP-MIT.txt.
  Local changes: a ZBM_DECODE_ONLY guard excludes the encoder from the build;
  bitmap repetition stops when the declared chunk output is complete. Our
  decmpfs wrapper additionally checks all chunk lengths, total output, the
  terminator and consumed input before invoking the decoder.

Compressed-file output is limited to the declared logical length and one
64 KiB compression unit at a time. Exact output length and decoder status are checked. Native type-4 zlib units
may finish at a sync-flush boundary; resource-based LZVN units may contain
padding after their end-of-stream opcode. Inline streams require completion
and exhausted input. Unsupported decmpfs methods are reported explicitly.

HFVExplorer independently constructs type-3/type-4 decmpfs output. Inline values
respect the 3802-byte header-inclusive limit. Type-4 output has 64 KiB zlib units
(or 0xFF stored units), mixed-endian block tables and a single unnamed cmpf
resource map. A planning pass bounds the 32-bit resource offsets; a second pass
checks each encoded unit's length and checksum before publication. No APSL
implementation was incorporated; the existing GPL-compatible zlib license
covers the additional unmodified encoder files from the same pinned commit.
