# SPDX-License-Identifier: GPL-2.0-or-later
"""Independently inspect the GPT journal and timestamp fixtures from --other-features."""
import pathlib
import struct
import sys
import uuid
import zlib

root = pathlib.Path(sys.argv[1])
image = (root / 'external.dsk').read_bytes()
disk = (root / 'journal.img').read_bytes()
sectors = len(disk) // 512
assert disk[510:512] == b'\x55\xaa' and disk[450] == 0xee
tables = []
for lba in (1, sectors - 1):
    header = bytearray(disk[lba * 512:(lba + 1) * 512])
    assert header[:8] == b'EFI PART'
    size, crc = struct.unpack_from('<II', header, 12)
    assert size == 92
    header[16:20] = bytes(4)
    assert zlib.crc32(header[:size]) == crc
    current, alternate, first, last = struct.unpack_from('<QQQQ', header, 24)
    assert current == lba and alternate == (sectors - 1 if lba == 1 else 1)
    table_lba, count, stride, table_crc = struct.unpack_from('<QIII', header, 72)
    table = disk[table_lba * 512:table_lba * 512 + count * stride]
    assert len(table) == count * stride and zlib.crc32(table) == table_crc
    tables.append(table)
assert tables[0] == tables[1]
entry = tables[0][:128]
assert str(uuid.UUID(bytes_le=entry[:16])).upper() == '4A6F7572-6E61-11AA-AA11-00306543ECAC'
identity = str(uuid.UUID(bytes_le=entry[16:32])).upper()
begin, end = struct.unpack_from('<QQ', entry, 32)
assert first <= begin <= end <= last

header = image[1024:1536]
assert header[:4] == b'HX\x00\x05' and header == image[-1024:-512]
flags, _, jib_block = struct.unpack_from('>III', header, 4)
assert flags & 0x20002000 == 0x20002000
block_size = struct.unpack_from('>I', header, 40)[0]
jib = image[jib_block * block_size:jib_block * block_size + 180]
assert struct.unpack_from('>I', jib)[0] & 3 == 2
assert jib[52:88].decode('ascii').upper() == identity and jib[88] == 0
offset, size = struct.unpack_from('>QQ', jib, 36)
assert offset + size <= (end - begin + 1) * 512
log = disk[begin * 512 + offset:begin * 512 + offset + size]
assert log[4:8] in (b'\x78\x56\x34\x12', b'\x12\x34\x56\x78')
order = '<' if log[4:8] == b'\x78\x56\x34\x12' else '>'
magic, endian, start, finish, length = struct.unpack_from(order + 'IIQQQ', log)
assert magic == 0x4a4e4c78 and endian == 0x12345678 and start == finish and length == size

# This small fixture's catalog is inline and contiguous; parse its leaf records.
catalog_size = struct.unpack_from('>Q', header, 272)[0]
catalog_block = struct.unpack_from('>I', header, 288)[0]
catalog = image[catalog_block * block_size:catalog_block * block_size + catalog_size]
node_size = struct.unpack_from('>H', catalog, 32)[0]
leaf = struct.unpack_from('>I', catalog, 24)[0]
found = False
while leaf:
    node = catalog[leaf * node_size:(leaf + 1) * node_size]
    count = struct.unpack_from('>H', node, 10)[0]
    for i in range(count):
        at = struct.unpack_from('>H', node, node_size - 2 * (i + 1))[0]
        record = node[at:]
        key_size = struct.unpack_from('>H', record)[0]
        name_length = struct.unpack_from('>H', record, 6)[0]
        name = record[8:8 + name_length * 2].decode('utf-16-be')
        value = record[(key_size + 3) & ~1:]
        kind, attributes = struct.unpack_from('>HH', value)
        if kind == 2 and name == 'File':
            assert attributes & 0x1000
            assert struct.unpack_from('>II', value, 12) == (0, 0xffffffff)
            found = True
    leaf = struct.unpack_from('>I', node)[0]
assert found
print('PASS: independent GPT CRCs/GUIDs/bounds, JIB pairing, clean external journal and native 1970/2106 timestamp encoding')
