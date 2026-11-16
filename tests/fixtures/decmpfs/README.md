The gzipped HFS+ image and SHA-256 manifest are retained macOS-written fixtures
from https://github.com/abrignoni/qnxprobe, commit
`0c18009ee0e2dc3242486dc010e4d940f13747b3`.

Upstream `tools/make_decmpfs_fixture.py` creates the filesystem on macOS,
installs each decmpfs representation and reads each file back through macOS's
mounted filesystem. The manifest hashes are of that native readback. Methods
1, 3, 4, 7, 8, 9, 10, 11 and 12 must export with matching hashes here. Methods
13/14 (LZBITMAP) must fail explicitly. Files with a decmpfs attribute but no
UF_COMPRESSED flag must export their ordinary data forks.

Run `tests/test-hfsplus-compression.ps1`; `build-x64.ps1 -TestHfsPlus` includes
this check. Only newly created ordinary test files are used. There is no
macOS, Python, WSL or mounting requirement to run it on Windows.

Copyright (c) 2026 Alexis Brignoni. Distributed under the MIT license in
`LICENSE`. These are test data, not a third-party filesystem implementation.
