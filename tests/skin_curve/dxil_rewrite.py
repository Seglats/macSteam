"""Rewrite one DXIL construct the way vkd3d-proton's dxil-spirv handles it.

THE FINALS' skin shaders fill a local `alloca [N x float]` from consecutive constant-buffer
floats, then read it at a runtime index with no range check (tests/rosetta_nt/R15_SKIN_CURVE.md).
Out-of-range indices are undefined in DXIL: Proton reads the constant buffer directly
(bounds-checked), D3DMetal keeps the private array and reads past it on Apple GPUs.

Modes:
  clamp     insert idx = clamp(idx, 0, N-1) before the dynamic read (proof of cause)
  reencode  decode and re-encode the function with nothing inserted (encoder check)

The rewriter only inserts instructions into the shader's function body and renumbers the
operands behind them. Anything it does not fully understand leaves the shader unchanged.
"""
import struct
import sys

import bitstream as bs
import dxbc

MODULE_BLOCK, CONSTANTS_BLOCK, FUNCTION_BLOCK, VST_BLOCK = 8, 11, 12, 14
METADATA_BLOCK, METADATA_ATTACHMENT, TYPE_BLOCK, USELIST_BLOCK = 15, 16, 17, 18

# instruction codes (LLVM 3.7 FUNC_CODE_*)
DECLAREBLOCKS, BINOP, CAST, EXTRACTELT, INSERTELT, SHUFFLEVEC, CMP, RET, BR, SWITCH = 1, 2, 3, 6, 7, 8, 9, 10, 11, 12
UNREACHABLE, PHI, ALLOCA, LOAD, EXTRACTVAL, INSERTVAL, CMP2, VSELECT = 15, 16, 19, 20, 26, 27, 28, 29
DEBUG_LOC_AGAIN, CALL, DEBUG_LOC, FENCE, ATOMICRMW, GEP, STORE, CMPXCHG = 33, 34, 35, 36, 38, 43, 44, 46
ICMP_SGT, ICMP_SLT = 38, 40


class Unsupported(Exception):
    pass


class Module:
    """Types and the module-level value list (globals, functions, constants)."""

    def __init__(self, module_block):
        self.types = []                      # (kind, data)
        self.values = []                     # (kind, type id, extra)
        self.functions = []                  # value ids of functions with bodies, in order
        consts = None
        for it in module_block.items:
            if isinstance(it, bs.Block):
                if it.block_id == TYPE_BLOCK:
                    self._types(it)
                elif it.block_id == CONSTANTS_BLOCK:
                    consts = it
            elif it.code == 7:               # GLOBALVAR [ptrtype or valtype, isconst, ...]
                self.values.append(('global', it.ops[0], None))
            elif it.code == 8:               # FUNCTION [type, cc, isproto, ...]
                fty = it.ops[0]
                if self.types[fty][0] == 'pointer':
                    fty = self.types[fty][1][0]
                self.values.append(('function', fty, None))
                if not it.ops[2]:
                    self.functions.append(len(self.values) - 1)
            elif it.code in (9, 14):         # ALIAS
                self.values.append(('alias', it.ops[0], None))
        self.num_globals = len(self.values)
        if consts is not None:
            self._constants(consts, self.values)

    def _types(self, block):
        for it in block.items:
            if isinstance(it, bs.Block) or it.code in (-1, 1, 19):   # define, NUMENTRY, STRUCT_NAME
                continue
            kinds = {2: 'void', 3: 'float', 4: 'double', 5: 'label', 6: 'opaque', 7: 'int',
                     8: 'pointer', 10: 'half', 11: 'array', 12: 'vector', 16: 'metadata',
                     18: 'struct', 20: 'struct', 21: 'function'}
            if it.code not in kinds:
                raise Unsupported('type code %d' % it.code)
            self.types.append((kinds[it.code], it.ops))

    def _constants(self, block, values):
        cur = None
        for it in block.items:
            if isinstance(it, bs.Block) or it.code == -1:
                continue
            if it.code == 1:                 # SETTYPE
                cur = it.ops[0]
                continue
            extra = None
            if it.code == 4:                 # INTEGER (signed VBR)
                v = it.ops[0]
                extra = -(v >> 1) if v & 1 else v >> 1
            elif it.code == 2:               # NULL
                extra = 0
            values.append(('const', cur, extra))

    def int_type(self, width):
        for i, (k, d) in enumerate(self.types):
            if k == 'int' and d[0] == width:
                return i
        return None

    def find_const(self, values, type_id, value):
        for i, (k, t, extra) in enumerate(values):
            if k == 'const' and t == type_id and extra == value:
                return i
        return None


def _fn_ret_void(mod, fty):
    k, d = mod.types[fty]
    if k != 'function':
        raise Unsupported('callee type')
    return mod.types[d[1]][0] == 'void'


class Inst:
    """One instruction record with its relative value operands decoded to absolute ids."""
    __slots__ = ('code', 'ops', 'refs', 'value', 'signed', '_old_cur')

    def __init__(self, code, ops):
        self.code, self.ops, self.refs, self.value, self.signed = code, list(ops), [], None, False


def decode_function(mod, fblock, fn_value_id):
    """Return (values, insts, first_inst_value) for a function block."""
    values = list(mod.values)
    fty = mod.values[fn_value_id][1]
    params = mod.types[fty][1][2:]
    for p in params:
        values.append(('arg', p, None))
    for it in fblock.items:
        if isinstance(it, bs.Block):
            if it.block_id == CONSTANTS_BLOCK:
                mod._constants(it, values)
            elif it.block_id in (METADATA_BLOCK, USELIST_BLOCK):
                raise Unsupported('function-level block %d' % it.block_id)
    inst_num = len(values)
    first = inst_num
    insts = []
    for it in fblock.items:
        if isinstance(it, bs.Block) or it.code == -1:
            continue
        if it.code in (DECLAREBLOCKS, DEBUG_LOC, DEBUG_LOC_AGAIN):
            insts.append(Inst(it.code, it.ops))
            insts[-1].value = None
            continue
        inst = Inst(it.code, it.ops)
        ops, n = inst.ops, len(inst.ops)

        def rel(i, with_type=True):
            v = (inst_num - ops[i]) & 0xffffffff
            inst.refs.append(i)
            fwd = v >= inst_num
            return i + (2 if (with_type and fwd) else 1)

        c = it.code
        produces = True
        if c == BINOP:
            i = rel(0); i = rel(i, False)
        elif c == CAST:
            rel(0)
        elif c in (CMP, CMP2):
            i = rel(0); rel(i, False)
        elif c == VSELECT:
            i = rel(0); i = rel(i, False); rel(i)
        elif c == EXTRACTELT:
            i = rel(0); rel(i)
        elif c == INSERTELT:
            i = rel(0); i = rel(i, False); rel(i)
        elif c == SHUFFLEVEC:
            i = rel(0); i = rel(i, False); rel(i, False)
        elif c == GEP:
            i = 2
            while i < n:
                i = rel(i)
        elif c == LOAD:
            rel(0)
        elif c == STORE:
            i = rel(0); rel(i)
            produces = False
        elif c == EXTRACTVAL:
            rel(0)
        elif c == INSERTVAL:
            i = rel(0); rel(i)
        elif c == ALLOCA:
            pass                              # size operand is an absolute constant id
        elif c == RET:
            if n:
                rel(0)
            produces = False
        elif c == BR:
            if n == 3:
                rel(2, False)
            produces = False
        elif c == SWITCH:
            rel(1, False)                     # case values are absolute constant ids
            produces = False
        elif c == UNREACHABLE:
            produces = False
        elif c == PHI:
            inst.signed = True
            for i in range(1, n, 2):
                inst.refs.append(i)
        elif c == ATOMICRMW:
            i = rel(0); rel(i, False)
        elif c == CMPXCHG:
            i = rel(0); i = rel(i); rel(i, False)
        elif c == CALL:
            cc = ops[1]
            i = 2
            explicit = bool(cc & (1 << 15))
            if explicit:
                fty_c = ops[i]; i += 1
            callee = (inst_num - ops[i]) & 0xffffffff
            i = rel(i)
            if not explicit:
                fty_c = values[callee][1]
                if mod.types[fty_c][0] == 'pointer':
                    fty_c = mod.types[fty_c][1][0]
            ftype = mod.types[fty_c]
            if ftype[0] != 'function' or ftype[1][0]:
                raise Unsupported('vararg or odd call')
            for pty in ftype[1][2:]:
                if mod.types[pty][0] == 'label':
                    i += 1
                else:
                    i = rel(i, False)
            produces = not _fn_ret_void(mod, fty_c)
        elif c == FENCE:
            produces = False
        else:
            raise Unsupported('instruction code %d' % c)
        if produces:
            inst.value = inst_num
            values.append(('inst', None, c))
            inst_num += 1
        insts.append(inst)
    return values, insts, first


def _abs(inst, slot, cur):
    raw = inst.ops[slot]
    if inst.signed:
        r = -(raw >> 1) if raw & 1 else raw >> 1
        return cur - r
    return (cur - raw) & 0xffffffff


def _rel(inst, absval, cur):
    if inst.signed:
        r = cur - absval
        return (r << 1) if r >= 0 else ((-r) << 1) | 1
    return (cur - absval) & 0xffffffff


def find_curves(mod, values, insts):
    """Dynamic reads of a local float array: (gep index, array length, index value id)."""
    fty = mod.int_type(32)
    out = []
    allocas = {}
    for k, inst in enumerate(insts):
        if inst.code == ALLOCA:
            t = mod.types[inst.ops[0]]
            if t[0] == 'array' and mod.types[t[1][1]][0] == 'float':
                allocas[inst.value] = t[1][0]
    cur = None
    for k, inst in enumerate(insts):
        if inst.code != GEP:
            continue
        cur_num = inst.value
        refs = inst.refs
        base = _abs(inst, refs[0], cur_num)
        if base not in allocas or len(refs) != 3:
            continue
        idx = _abs(inst, refs[2], cur_num)
        if values[idx][0] == 'const':
            continue
        out.append((k, allocas[base], idx, refs[2]))
    return out


def rewrite_function(mod, fblock, fn_value_id, mode):
    values, insts, first = decode_function(mod, fblock, fn_value_id)
    curves = find_curves(mod, values, insts)
    if mode == 'reencode':
        curves = []                            # re-encode only: proves the encoder
    elif not curves:
        return None, 0
    i32 = mod.int_type(32)
    i1 = mod.int_type(1)
    plan = []                                  # (inst index, [new insts], gep slot, idx id)
    for k, length, idx, slot in curves:
        c0 = mod.find_const(values, i32, 0)
        cmax = mod.find_const(values, i32, length - 1)
        if c0 is None or cmax is None or i1 is None:
            raise Unsupported('clamp constants missing')
        plan.append((k, idx, slot, c0, cmax))
    # Build the new instruction list. New value ids are assigned in order; every old
    # operand is re-expressed against the new numbering.
    out = []                                   # (code, ops, refs-as-absolute-new, produces)
    remap = {}                                 # old value id -> new value id
    shift = 0
    by_index = {p[0]: p for p in plan}
    next_value = first
    for k, inst in enumerate(insts):
        if k in by_index:
            _, idx, slot, c0, cmax = by_index[k]
            n_idx = remap.get(idx, idx)
            # c1 = icmp slt idx, 0 ; s1 = select c1, 0, idx ; c2 = icmp sgt s1, max ; s2 = select c2, max, s1
            seq = []
            c1 = next_value; seq.append((CMP2, [('v', n_idx), ('v', c0), ('lit', ICMP_SLT)])); next_value += 1
            s1 = next_value; seq.append((VSELECT, [('v', c0), ('v', n_idx), ('v', c1)])); next_value += 1
            c2 = next_value; seq.append((CMP2, [('v', s1), ('v', cmax), ('lit', ICMP_SGT)])); next_value += 1
            s2 = next_value; seq.append((VSELECT, [('v', cmax), ('v', s1), ('v', c2)])); next_value += 1
            out.extend(('new', code, ops) for code, ops in seq)
            override = (slot, s2)
        else:
            override = None
        if inst.value is not None:
            remap[inst.value] = next_value
            next_value += 1
        out.append(('old', inst, override))
    return (values, insts, first, out, remap), (len(plan) or (1 if mode == 'reencode' else 0))


def encode_function(mod, fblock, fn_value_id, rewritten, w, data, parent_width):
    values, insts, first, out, remap = rewritten
    width = fblock.abbrev_width
    w.write(bs.ENTER_SUBBLOCK, parent_width)
    w.vbr(FUNCTION_BLOCK, 8)
    w.vbr(width, 4)
    w.align32()
    len_pos = w.pos
    w.write(0, 32)
    body_start = w.pos
    inst_map = {}                               # old instruction index -> new instruction index
    # Leading sub-blocks (constants) and DECLAREBLOCKS come first, as in the original.
    insts_iter = iter(out)
    new_inst_index = 0
    cur = first
    old_inst_index = 0
    items = [it for it in fblock.items]
    pending_subblocks_after = []
    emitted = False
    for it in items:
        if isinstance(it, bs.Block):
            if it.block_id in (VST_BLOCK, METADATA_ATTACHMENT):
                pending_subblocks_after.append(it)
                continue
            _copy_block(w, data, it, width)
            continue
        if it.code == -1:
            continue                            # local abbreviations are not needed
        if it.code == DECLAREBLOCKS:
            bs.write_unabbrev(w, width, it.code, it.ops)
            continue                            # before the function's constants block, as LLVM expects
        if not emitted:
            new_inst_index, cur = _emit_insts(w, width, out, first, remap, inst_map)
            emitted = True
    for it in pending_subblocks_after:
        if it.block_id == METADATA_ATTACHMENT:
            _emit_attachments(w, width, it, inst_map)
        elif it.block_id == VST_BLOCK:
            _emit_vst(w, width, it, remap)
    w.write(bs.END_BLOCK, width)
    w.align32()
    end = w.pos
    nwords = (end - body_start) // 32
    w.bits[len_pos // 8:len_pos // 8 + 4] = struct.pack('<I', nwords)


def _emit_insts(w, width, out, first, remap, inst_map):
    cur = first
    new_index = 0
    old_index = 0
    for entry in out:
        if entry[0] == 'new':
            _, code, ops = entry
            enc = []
            for kind, v in ops:
                enc.append((cur - v) & 0xffffffff if kind == 'v' else v)
            bs.write_unabbrev(w, width, code, enc)
            cur += 1
            new_index += 1
            continue
        _, inst, override = entry
        if inst.code == DECLAREBLOCKS:
            continue                            # written by encode_function before the constants
        if inst.code in (DEBUG_LOC, DEBUG_LOC_AGAIN):
            bs.write_unabbrev(w, width, inst.code, inst.ops)
            continue
        ops = list(inst.ops)
        old_cur = inst.value if inst.value is not None else None
        for slot in inst.refs:
            # Old current number: the value id this instruction had, or the next one.
            oc = inst._old_cur
            a = _abs(inst, slot, oc)
            na = remap.get(a, a)
            ops[slot] = _rel(inst, na, cur)
        if override:
            slot, newval = override
            ops[slot] = (cur - newval) & 0xffffffff
        bs.write_unabbrev(w, width, inst.code, ops)
        inst_map[old_index] = new_index
        old_index += 1
        new_index += 1
        if inst.value is not None:
            cur += 1
    return new_index, cur


def _copy_block(w, data, blk, width):
    w.write(bs.ENTER_SUBBLOCK, width)
    w.vbr(blk.block_id, 8)
    w.vbr(blk.abbrev_width, 4)
    w.align32()
    w.write((blk.end - blk.content_start) // 32, 32)
    w.copy(data, blk.content_start, blk.end)


def _emit_attachments(w, width, blk, inst_map):
    w.write(bs.ENTER_SUBBLOCK, width)
    w.vbr(METADATA_ATTACHMENT, 8)
    w.vbr(3, 4)
    w.align32()
    lp = w.pos
    w.write(0, 32)
    start = w.pos
    for it in blk.items:
        if isinstance(it, bs.Block) or it.code == -1:
            continue
        ops = list(it.ops)
        if len(ops) % 2 == 1:                   # [instid, (kind, md)...]
            if ops[0] not in inst_map:
                raise Unsupported('attachment to unknown instruction')
            ops[0] = inst_map[ops[0]]
        bs.write_unabbrev(w, 3, it.code, ops)
    w.write(bs.END_BLOCK, 3)
    w.align32()
    w.bits[lp // 8:lp // 8 + 4] = struct.pack('<I', (w.pos - start) // 32)


def _emit_vst(w, width, blk, remap):
    w.write(bs.ENTER_SUBBLOCK, width)
    w.vbr(VST_BLOCK, 8)
    w.vbr(4, 4)
    w.align32()
    lp = w.pos
    w.write(0, 32)
    start = w.pos
    for it in blk.items:
        if isinstance(it, bs.Block) or it.code == -1:
            continue
        ops = list(it.ops)
        if it.code == 1:                        # VST_ENTRY [valueid, name...]
            ops[0] = remap.get(ops[0], ops[0])
        bs.write_unabbrev(w, 4, it.code, ops)
    w.write(bs.END_BLOCK, 4)
    w.align32()
    w.bits[lp // 8:lp // 8 + 4] = struct.pack('<I', (w.pos - start) // 32)


def rewrite_bitcode(bc, mode='clamp'):
    top, info = bs.parse(bc, 32)
    module = next(it for it in top.items if isinstance(it, bs.Block) and it.block_id == MODULE_BLOCK)
    mod = Module(module)
    fblocks = [it for it in module.items if isinstance(it, bs.Block) and it.block_id == FUNCTION_BLOCK]
    if len(fblocks) != 1 or len(mod.functions) != 1:
        raise Unsupported('expected one function body')
    fblock = fblocks[0]
    result, count = rewrite_function(mod, fblock, mod.functions[0], mode)
    if not count:
        return None, 0
    values, insts, first, out, remap = result
    # remember each old instruction's own "current" number for decoding its operands
    cur = first
    for inst in insts:
        inst._old_cur = cur
        if inst.value is not None:
            cur += 1
    w = bs.BitWriter()
    w.copy(bc, 0, module.len_word_pos)
    mod_len_pos = w.pos
    w.write(0, 32)
    w.copy(bc, module.content_start, fblock.start)
    encode_function(mod, fblock, mod.functions[0], result, w, bc, module.abbrev_width)
    w.copy(bc, fblock.end, module.end)
    new_len = (w.pos - module.content_start) // 32
    w.bits[mod_len_pos // 8:mod_len_pos // 8 + 4] = struct.pack('<I', new_len)
    w.copy(bc, module.end, len(bc) * 8)
    return w.getvalue(), count


def rewrite_container(blob, mode='clamp'):
    c = dxbc.Container(blob)
    part = c.part(b'DXIL')
    if part is None:
        return None, 0
    fields, bc = dxbc.program_bitcode(part[1])
    new_bc, count = rewrite_bitcode(bc, mode)
    if not count:
        return None, 0
    part[1] = dxbc.program_part(fields, new_bc)
    return c.build(), count


if __name__ == '__main__':
    src, dst = sys.argv[1], sys.argv[2]
    out, n = rewrite_container(open(src, 'rb').read(), sys.argv[3] if len(sys.argv) > 3 else 'clamp')
    if out is None:
        print('no match'); sys.exit(1)
    open(dst, 'wb').write(out)
    print('rewrote %d read(s)' % n)
