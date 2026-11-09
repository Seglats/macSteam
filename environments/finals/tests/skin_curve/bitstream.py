"""LLVM bitstream reader and writer, enough for DXIL (LLVM 3.7 bitcode).

The reader parses blocks, abbreviations (including BLOCKINFO) and records, and keeps the
bit position of every item, so a caller can copy untouched parts of the stream bit for
bit and re-encode only what it changes. The writer emits unabbreviated records, which
every bitcode reader accepts.
"""

END_BLOCK, ENTER_SUBBLOCK, DEFINE_ABBREV, UNABBREV_RECORD = 0, 1, 2, 3
BLOCKINFO_BLOCK_ID = 0
FIXED, VBR, ARRAY, CHAR6, BLOB = 1, 2, 3, 4, 5


class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0                      # bit position
        self.end = len(data) * 8

    def read(self, n):
        value, shift = 0, 0
        while n:
            byte = self.data[self.pos >> 3]
            off = self.pos & 7
            take = min(8 - off, n)
            value |= ((byte >> off) & ((1 << take) - 1)) << shift
            shift += take
            n -= take
            self.pos += take
        return value

    def vbr(self, n):
        hi = 1 << (n - 1)
        value, shift = 0, 0
        while True:
            piece = self.read(n)
            value |= (piece & (hi - 1)) << shift
            if not piece & hi:
                return value
            shift += n - 1

    def align32(self):
        self.pos = (self.pos + 31) & ~31


class BitWriter:
    def __init__(self):
        self.bits = bytearray()
        self.cur = 0
        self.nbits = 0

    @property
    def pos(self):
        return len(self.bits) * 8 + self.nbits

    def write(self, value, n):
        while n:
            take = min(8 - self.nbits, n)
            self.cur |= (value & ((1 << take) - 1)) << self.nbits
            value >>= take
            n -= take
            self.nbits += take
            if self.nbits == 8:
                self.bits.append(self.cur)
                self.cur = 0
                self.nbits = 0

    def vbr(self, value, n):
        hi = 1 << (n - 1)
        while value >= hi:
            self.write((value & (hi - 1)) | hi, n)
            value >>= n - 1
        self.write(value, n)

    def align32(self):
        while self.pos & 31:
            self.write(0, 1)

    def copy(self, reader_data, start, end):
        """Copy bits [start, end) of another stream."""
        r = BitReader(reader_data)
        r.pos = start
        while r.pos < end:
            n = min(32, end - r.pos)
            self.write(r.read(n), n)

    def patch32(self, bitpos, value):
        assert bitpos % 8 == 0 and self.nbits == 0 or bitpos + 32 <= len(self.bits) * 8
        i = bitpos // 8
        self.bits[i:i + 4] = value.to_bytes(4, 'little')

    def getvalue(self):
        assert self.nbits == 0
        return bytes(self.bits)


class Record:
    __slots__ = ('code', 'ops', 'start', 'end', 'abbrev')

    def __init__(self, code, ops, start, end, abbrev):
        self.code, self.ops, self.start, self.end, self.abbrev = code, ops, start, end, abbrev


class Block:
    __slots__ = ('block_id', 'abbrev_width', 'items', 'start', 'content_start', 'end',
                 'len_word_pos')

    def __init__(self, block_id, abbrev_width, start):
        self.block_id, self.abbrev_width, self.start = block_id, abbrev_width, start
        self.items = []


def _read_abbrev(r):
    ops = []
    numops = r.vbr(5)
    i = 0
    while i < numops:
        if r.read(1):
            ops.append(('lit', r.vbr(8)))
        else:
            enc = r.read(3)
            if enc in (FIXED, VBR):
                ops.append((enc, r.vbr(5)))
            else:
                ops.append((enc, None))
        i += 1
    return ops


def _read_scalar(r, op):
    kind, val = op
    if kind == 'lit':
        return val
    if kind == FIXED:
        return r.read(val)
    if kind == VBR:
        return r.vbr(val)
    if kind == CHAR6:
        return r.read(6)
    raise ValueError('bad scalar abbrev op %r' % (op,))


def _read_abbrev_record(r, abbrev):
    vals = []
    i = 0
    while i < len(abbrev):
        op = abbrev[i]
        if op[0] == ARRAY:
            n = r.vbr(6)
            elt = abbrev[i + 1]
            vals.extend(_read_scalar(r, elt) for _ in range(n))
            i += 2
            continue
        if op[0] == BLOB:
            n = r.vbr(6)
            r.align32()
            vals.append(bytes(r.read(8) for _ in range(n)))
            r.align32()
        else:
            vals.append(_read_scalar(r, op))
        i += 1
    return vals[0], vals[1:]


def parse(data, start_bit, abbrev_width=2, blockinfo=None, stop=None):
    """Parse items at the given level until END_BLOCK or the end of the data."""
    if blockinfo is None:
        blockinfo = {}
    r = BitReader(data)
    r.pos = start_bit
    top = Block(None, abbrev_width, start_bit)
    top.content_start = start_bit
    _parse_block(r, top, blockinfo, toplevel=True)
    return top, blockinfo


def _parse_block(r, block, blockinfo, toplevel=False):
    abbrevs = list(blockinfo.get(block.block_id, [])) if block.block_id is not None else []
    cur_bid = None                         # for BLOCKINFO SETBID
    while r.pos < r.end:
        item_start = r.pos
        aid = r.read(block.abbrev_width)
        if aid == END_BLOCK:
            r.align32()
            block.end = r.pos
            return
        if aid == ENTER_SUBBLOCK:
            bid = r.vbr(8)
            width = r.vbr(4)
            r.align32()
            len_pos = r.pos
            nwords = r.read(32)
            sub = Block(bid, width, item_start)
            sub.len_word_pos = len_pos
            sub.content_start = r.pos
            body_end = r.pos + nwords * 32
            if bid == BLOCKINFO_BLOCK_ID:
                _parse_blockinfo(r, sub, blockinfo)
            else:
                _parse_block(r, sub, blockinfo)
            assert sub.end == body_end, 'block %d length mismatch' % bid
            block.items.append(sub)
            continue
        if aid == DEFINE_ABBREV:
            abbrevs.append(_read_abbrev(r))
            block.items.append(Record(-1, None, item_start, r.pos, 'define'))
            continue
        if aid == UNABBREV_RECORD:
            code = r.vbr(6)
            n = r.vbr(6)
            ops = [r.vbr(6) for _ in range(n)]
            block.items.append(Record(code, ops, item_start, r.pos, None))
            continue
        code, ops = _read_abbrev_record(r, abbrevs[aid - 4])
        block.items.append(Record(code, ops, item_start, r.pos, aid))
    block.end = r.pos


def _parse_blockinfo(r, block, blockinfo):
    cur = None
    while True:
        item_start = r.pos
        aid = r.read(block.abbrev_width)
        if aid == END_BLOCK:
            r.align32()
            block.end = r.pos
            return
        if aid == DEFINE_ABBREV:
            blockinfo.setdefault(cur, []).append(_read_abbrev(r))
            continue
        if aid == UNABBREV_RECORD:
            code = r.vbr(6)
            n = r.vbr(6)
            ops = [r.vbr(6) for _ in range(n)]
            if code == 1:                  # SETBID
                cur = ops[0]
            block.items.append(Record(code, ops, item_start, r.pos, None))
            continue
        raise ValueError('abbreviated record in BLOCKINFO')


def write_unabbrev(w, width, code, ops):
    w.write(UNABBREV_RECORD, width)
    w.vbr(code, 6)
    w.vbr(len(ops), 6)
    for v in ops:
        w.vbr(v, 6)
