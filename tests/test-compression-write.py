# SPDX-License-Identifier: GPL-2.0-or-later
"""Validate emitted decmpfs framing using Python's independent zlib decoder."""
import pathlib
import struct
import subprocess
import sys
import zlib

exe, directory = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
directory.mkdir(exist_ok=False, parents=True)
subprocess.run([str(exe), '--compression-encoding-fixture', str(directory / 'file')],
               check=True, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
for mode in range(3):
    base = directory / ('file' + str(mode))
    plain = base.with_suffix('.raw').read_bytes()
    attr = base.with_suffix('.attr').read_bytes()
    resource = base.with_suffix('.rsrc').read_bytes()
    magic, kind, size = struct.unpack('<IIQ', attr[:16])
    assert magic == 0x636d7066 and size == len(plain)

    def decode(payload):
        if payload[0] & 15 == 15:
            return payload[1:]
        stream = zlib.decompressobj()
        output = stream.decompress(payload) + stream.flush()
        assert stream.eof and not stream.unused_data and not stream.unconsumed_tail
        return output

    if kind == 3:
        assert len(attr) <= 3802 and not resource
        output = decode(attr[16:])
    else:
        assert kind == 4 and len(attr) == 16
        data, mapping, data_size, map_size = struct.unpack('>IIII', resource[:16])
        assert data == 256 and mapping == data + data_size and map_size == 50
        assert len(resource) == mapping + map_size
        # Independent resource-map interpretation: one unnamed cmpf resource, ID 1.
        type_at, names_at = struct.unpack_from('>HH', resource, mapping + 24)
        assert type_at == 28 and names_at == 50
        count, tag, count_minus_one, ref_at = struct.unpack_from('>H4sHH', resource, mapping + type_at)
        assert count == count_minus_one == 0 and tag == b'cmpf' and ref_at == 10
        ref = mapping + type_at + ref_at
        rid, name = struct.unpack_from('>HH', resource, ref)
        assert rid == 1 and name == 0xffff and resource[ref + 4:ref + 8] == bytes(4)
        payload_size = struct.unpack_from('>I', resource, data)[0]
        assert payload_size + 4 == data_size
        blocks = struct.unpack_from('<I', resource, data + 4)[0]
        assert blocks == (size + 65535) // 65536
        output = bytearray()
        end = 4 + 8 * blocks
        for block in range(blocks):
            offset, length = struct.unpack_from('<II', resource, data + 8 + 8 * block)
            assert offset == end and offset + length <= payload_size
            unit = decode(resource[data + 4 + offset:data + 4 + offset + length])
            assert len(unit) == min(65536, size - block * 65536)
            output.extend(unit)
            end = offset + length
        assert end == payload_size
    assert output == plain
    print(f'PASS: independent decmpfs type {kind}, {size} bytes, resource map and zlib/stored units')
# Independent framing checks for every supported output type. Codec round trips
# and journal failures are covered by the C++ suite; this parser checks the disk
# layout separately, and independently decodes zlib and stored payloads.
seen = set()
for path in sorted(directory.glob('file-codec*.attr')):
    attr = path.read_bytes()
    plain = path.with_suffix('.raw').read_bytes()
    resource = path.with_suffix('.rsrc').read_bytes()
    magic, kind, size = struct.unpack('<IIQ', attr[:16])
    assert magic == 0x636d7066 and size == len(plain)
    seen.add(kind)
    if kind in (1, 3, 7, 9, 11, 13):
        assert len(attr) <= 3802 and not resource
        units = [attr[16:]]
    elif kind == 4:
        data, mapping, data_size, map_size = struct.unpack('>IIII', resource[:16])
        assert data == 256 and mapping == data + data_size and len(resource) == mapping + map_size
        assert resource[mapping + 30:mapping + 34] == b'cmpf' and map_size == 50
        count = struct.unpack_from('<I', resource, data + 4)[0]
        assert count == (size + 65535) // 65536
        units = []
        end = 4 + count * 8
        for i in range(count):
            at, length = struct.unpack_from('<II', resource, data + 8 + i * 8)
            assert at == end and length and at + length <= data_size - 4
            units.append(resource[data + 4 + at:data + 4 + at + length])
            end = at + length
        assert end == data_size - 4
    else:
        assert kind in (8, 10, 12, 14) and len(attr) == 16
        count = (size + 65535) // 65536
        offsets = struct.unpack_from('<' + 'I' * (count + 1), resource)
        assert offsets[0] == (count + 1) * 4 and offsets[-1] == len(resource)
        assert all(a < b for a, b in zip(offsets, offsets[1:]))
        units = [resource[a:b] for a, b in zip(offsets, offsets[1:])]
    for i, unit in enumerate(units):
        expected = plain[i * 65536:(i + 1) * 65536]
        assert len(unit) <= 65537
        if kind == 1:
            assert unit == expected
        elif kind in (9, 10):
            assert unit[:1] == b'\xcc' and unit[1:] == expected
        elif kind in (3, 4):
            assert decode(unit) == expected
        elif unit[0] == (6 if kind in (7, 8) else 255):
            assert unit[1:] == expected
        elif kind in (7, 8):
            assert unit[-8:] == b'\x06' + bytes(7)
        elif kind in (11, 12):
            assert unit[:3] == b'bvx' and unit[-4:] == b'bvx$'
        else:
            assert unit[:4] == b'ZBM\x09'
            at, total = 4, 0
            while at < len(unit):
                length = int.from_bytes(unit[at:at + 3], 'little')
                output = int.from_bytes(unit[at + 3:at + 6], 'little')
                assert 6 <= length <= len(unit) - at and output <= 32768
                at += length
                total += output
                if output == 0:
                    assert length == 6 and at == len(unit)
                    break
            assert at == len(unit) and total == len(expected)
    print(f'PASS: independent type {kind} framing, {size} logical bytes')
assert seen == {1, 3, 4, 7, 8, 9, 10, 11, 12, 13, 14}, seen
