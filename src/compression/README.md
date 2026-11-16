These are codec sources only; the HFS+/HFSX filesystem implementation remains
HFVExplorer's own code. All decoders are linked statically, without a new DLL
or Windows API requirement. We do not build encoders or gzip/file APIs.

- `zlib/`: the inflate subset of zlib 1.4, upstream commit
  `99623ef7097352da7993ba614161fd6a311e046e`. Source: https://github.com/madler/zlib/tree/v1.4.
  Built with Z_SOLO and Z_PREFIX; the caller supplies bounded allocation.
  License: zlib/LICENSE and ../../licenses/zlib.txt.
- `lzfse/`: unmodified Apple LZFSE and LZVN decoder sources, commit
  `e634ca58b4821d9f3d560cdc6df5dec02ffc93fd`.
  Source: https://github.com/lzfse/lzfse.
  License: lzfse/LICENSE and ../../licenses/LZFSE-BSD.txt.

Compressed-file output is limited to the declared logical length and one
64 KiB compression unit at a time. Exact output length and decoder status are checked. Native type-4 zlib units
may finish at a sync-flush boundary; resource-based LZVN units may contain
padding after their end-of-stream opcode. Inline streams require completion
and exhausted input. Unsupported decmpfs methods are reported explicitly.
