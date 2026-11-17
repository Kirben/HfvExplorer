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
