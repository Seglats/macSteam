"""DXBC containers: parts, the DXIL program part, and the container checksum.

The checksum is MD5 with a different final block (vkd3d-shader checksum.c,
VKD3D_MD5_DXBC): the message bit count goes at the start of the last block and
(bits >> 2) | 1 at its end. It covers the container from byte 20 on.
"""
import math
import struct

_S = [7, 12, 17, 22] * 4 + [5, 9, 14, 20] * 4 + [4, 11, 16, 23] * 4 + [6, 10, 15, 21] * 4
_K = [int(abs(math.sin(i + 1)) * 2 ** 32) & 0xffffffff for i in range(64)]


def _rotl(x, c):
    return ((x << c) | (x >> (32 - c))) & 0xffffffff


def _transform(state, block):
    a, b, c, d = state
    m = struct.unpack('<16I', block)
    for i in range(64):
        if i < 16:
            f, g = (b & c) | (~b & d), i
        elif i < 32:
            f, g = (d & b) | (~d & c), (5 * i + 1) % 16
        elif i < 48:
            f, g = b ^ c ^ d, (3 * i + 5) % 16
        else:
            f, g = c ^ (b | ~d), (7 * i) % 16
        f = (f + a + _K[i] + m[g]) & 0xffffffff
        a, d, c = d, c, b
        b = (b + _rotl(f, _S[i])) & 0xffffffff
    return [(x + y) & 0xffffffff for x, y in zip(state, (a, b, c, d))]


def dxbc_checksum(data):
    state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476]
    full = len(data) - len(data) % 64
    for off in range(0, full, 64):
        state = _transform(state, data[off:off + 64])
    tail = bytearray(data[full:]) + b'\x80'
    bits = (len(data) * 8) & 0xffffffff
    count = len(tail)
    if 64 - count < 8:
        state = _transform(state, bytes(tail + bytes(64 - count)))
        last = bytearray(64)
    else:
        last = bytearray(4) + tail + bytes(60 - 4 - count) + bytearray(4)
    last[0:4] = struct.pack('<I', bits)
    last[60:64] = struct.pack('<I', (bits >> 2) | 1)
    state = _transform(state, bytes(last))
    return struct.pack('<4I', *state)


class Container:
    def __init__(self, blob):
        assert blob[:4] == b'DXBC'
        self.version = blob[20:24]
        count = struct.unpack_from('<I', blob, 28)[0]
        self.parts = []
        for i in range(count):
            off = struct.unpack_from('<I', blob, 32 + 4 * i)[0]
            fourcc = blob[off:off + 4]
            size = struct.unpack_from('<I', blob, off + 4)[0]
            self.parts.append([fourcc, bytes(blob[off + 8:off + 8 + size])])

    def part(self, fourcc):
        for p in self.parts:
            if p[0] == fourcc:
                return p
        return None

    def build(self):
        header = 32 + 4 * len(self.parts)
        offsets, body, off = [], bytearray(), header
        for fourcc, data in self.parts:
            offsets.append(off)
            chunk = fourcc + struct.pack('<I', len(data)) + data
            body += chunk
            off += len(chunk)
        out = bytearray(b'DXBC' + bytes(16) + self.version + struct.pack('<II', off, len(self.parts)))
        out += struct.pack('<%dI' % len(offsets), *offsets) + body
        out[4:20] = dxbc_checksum(bytes(out[20:]))
        return bytes(out)


def program_bitcode(part_data):
    """Split a DXIL program part into (header fields, bitcode)."""
    version, size_dwords, magic, dxil_version, bc_off, bc_size = struct.unpack_from('<IIIIII', part_data, 0)
    assert magic == 0x4C495844, 'not a DXIL program'
    return (version, dxil_version, bc_off), part_data[8 + bc_off:8 + bc_off + bc_size]


def program_part(fields, bitcode):
    version, dxil_version, bc_off = fields
    assert bc_off == 16
    data = struct.pack('<II', version, 0) + struct.pack('<IIII', 0x4C495844, dxil_version, bc_off, len(bitcode)) + bitcode
    data += bytes(-len(data) % 4)
    return data[:4] + struct.pack('<I', len(data) // 4) + data[8:]
