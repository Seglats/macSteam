/*
 * Rewrite one DXIL construct the way vkd3d-proton's dxil-spirv handles it.
 *
 * THE FINALS' skin shaders fill a local `alloca [N x float]` from constant-buffer floats
 * and read it at a runtime index without a range check (tests/rosetta_nt/R15_SKIN_CURVE.md).
 * Out-of-range indices are undefined in DXIL; Proton reads the constant buffer directly,
 * D3DMetal keeps the private array and reads past it on Apple GPUs.
 *
 * DXIL_REWRITE_CLAMP inserts idx = clamp(idx, 0, N-1) before each such read.
 *
 * C port of tests/skin_curve/dxil_rewrite.py; its output must match that script byte for
 * byte (tests/skin_curve/check_c_port.sh). Only the shader's function body is re-encoded;
 * everything else is copied bit for bit. Anything not fully understood returns 0 and the
 * caller keeps the original shader.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dxil_rewrite.h"

/* ---- bitstream ---------------------------------------------------------------------- */

struct reader { const uint8_t *data; size_t end, pos; int bad; };

static uint64_t rd(struct reader *r, unsigned n)
{
    uint64_t v = 0;
    unsigned shift = 0;
    while (n)
    {
        unsigned off, take;
        if (r->pos >= r->end) { r->bad = 1; return 0; }
        off = r->pos & 7;
        take = 8 - off < n ? 8 - off : n;
        v |= (uint64_t)((r->data[r->pos >> 3] >> off) & ((1u << take) - 1)) << shift;
        shift += take; n -= take; r->pos += take;
    }
    return v;
}

static uint64_t vbr(struct reader *r, unsigned n)
{
    uint64_t hi = 1ull << (n - 1), v = 0, piece;
    unsigned shift = 0;
    do
    {
        piece = rd(r, n);
        if (shift < 64) v |= (piece & (hi - 1)) << shift;
        shift += n - 1;
    } while ((piece & hi) && !r->bad);
    return v;
}

static void align32(struct reader *r) { r->pos = (r->pos + 31) & ~(size_t)31; }

struct writer { uint8_t *buf; size_t cap, pos; int bad; };

static void wr(struct writer *w, uint64_t v, unsigned n)
{
    while (n)
    {
        unsigned off = w->pos & 7, take = 8 - off < n ? 8 - off : n;
        size_t byte = w->pos >> 3;
        if (byte >= w->cap)
        {
            size_t cap = w->cap ? w->cap * 2 : 4096;
            uint8_t *b = realloc(w->buf, cap);
            if (!b) { w->bad = 1; return; }
            memset(b + w->cap, 0, cap - w->cap);
            w->buf = b; w->cap = cap;
        }
        w->buf[byte] |= (uint8_t)((v & ((1u << take) - 1)) << off);
        v >>= take; n -= take; w->pos += take;
    }
}

static void wvbr(struct writer *w, uint64_t v, unsigned n)
{
    uint64_t hi = 1ull << (n - 1);
    while (v >= hi) { wr(w, (v & (hi - 1)) | hi, n); v >>= n - 1; }
    wr(w, v, n);
}

static void walign32(struct writer *w) { while (w->pos & 31) wr(w, 0, 1); }

static void wcopy(struct writer *w, const uint8_t *data, size_t start, size_t end)
{
    struct reader r = { data, end, start, 0 };
    while (r.pos < end)
    {
        unsigned n = end - r.pos < 32 ? (unsigned)(end - r.pos) : 32;
        wr(w, rd(&r, n), n);
    }
}

static void wpatch32(struct writer *w, size_t bitpos, uint32_t v)
{
    memcpy(w->buf + bitpos / 8, &v, 4);
}

static void wrecord(struct writer *w, unsigned width, unsigned code, const uint64_t *ops, unsigned n)
{
    unsigned i;
    wr(w, 3, width);                    /* UNABBREV_RECORD */
    wvbr(w, code, 6);
    wvbr(w, n, 6);
    for (i = 0; i < n; i++) wvbr(w, ops[i], 6);
}

/* ---- parsed stream ------------------------------------------------------------------ */

enum { OP_LIT, OP_FIXED = 1, OP_VBR = 2, OP_ARRAY = 3, OP_CHAR6 = 4, OP_BLOB = 5 };
struct abbrev_op { int kind; uint64_t val; };
struct abbrev { unsigned n; struct abbrev_op op[32]; };

struct item
{
    int is_block;
    unsigned code;              /* record code, or block id */
    uint64_t *ops; unsigned nops;
    size_t start, end, content_start, len_pos;
    unsigned width;             /* blocks: abbrev width */
    struct item *items; unsigned nitems;
    int define;                 /* DEFINE_ABBREV item */
};

struct blockinfo { unsigned bid; struct abbrev *ab; unsigned n; };
struct parser { struct reader r; struct blockinfo bi[16]; unsigned nbi; int bad; };

static struct blockinfo *bi_get(struct parser *p, unsigned bid, int create)
{
    unsigned i;
    for (i = 0; i < p->nbi; i++) if (p->bi[i].bid == bid) return &p->bi[i];
    if (!create || p->nbi == 16) return NULL;
    p->bi[p->nbi].bid = bid; p->bi[p->nbi].ab = NULL; p->bi[p->nbi].n = 0;
    return &p->bi[p->nbi++];
}

static int read_abbrev(struct reader *r, struct abbrev *a)
{
    unsigned n = (unsigned)vbr(r, 5), i;
    if (n > 32) return 0;
    a->n = n;
    for (i = 0; i < n; i++)
    {
        if (rd(r, 1)) { a->op[i].kind = OP_LIT; a->op[i].val = vbr(r, 8); }
        else
        {
            a->op[i].kind = (int)rd(r, 3);
            a->op[i].val = (a->op[i].kind == OP_FIXED || a->op[i].kind == OP_VBR) ? vbr(r, 5) : 0;
            if (a->op[i].kind < 1 || a->op[i].kind > 5) return 0;
        }
    }
    return !r->bad;
}

static uint64_t read_scalar(struct reader *r, const struct abbrev_op *op)
{
    switch (op->kind)
    {
    case OP_LIT: return op->val;
    case OP_FIXED: return op->val ? rd(r, (unsigned)op->val) : 0;
    case OP_VBR: return op->val ? vbr(r, (unsigned)op->val) : 0;
    case OP_CHAR6: return rd(r, 6);
    }
    r->bad = 1;
    return 0;
}

static int push_op(struct item *it, uint64_t v)
{
    if (!(it->nops & 15))
    {
        uint64_t *o = realloc(it->ops, (it->nops + 16) * sizeof(*o));
        if (!o) return 0;
        it->ops = o;
    }
    it->ops[it->nops++] = v;
    return 1;
}

static struct item *push_item(struct item *blk)
{
    struct item *it;
    if (!(blk->nitems & 63))
    {
        struct item *n = realloc(blk->items, (blk->nitems + 64) * sizeof(*n));
        if (!n) return NULL;
        blk->items = n;
    }
    it = &blk->items[blk->nitems++];
    memset(it, 0, sizeof(*it));
    return it;
}

static int parse_block(struct parser *p, struct item *blk);

static int parse_blockinfo(struct parser *p, struct item *blk)
{
    struct blockinfo *cur = NULL;
    for (;;)
    {
        unsigned aid = (unsigned)rd(&p->r, blk->width);
        if (p->r.bad) return 0;
        if (aid == 0) { align32(&p->r); blk->end = p->r.pos; return 1; }
        if (aid == 2)
        {
            struct abbrev a, *n;
            if (!cur || !read_abbrev(&p->r, &a)) return 0;
            if (!(n = realloc(cur->ab, (cur->n + 1) * sizeof(*n)))) return 0;
            cur->ab = n; cur->ab[cur->n++] = a;
            continue;
        }
        if (aid == 3)
        {
            unsigned code = (unsigned)vbr(&p->r, 6), n = (unsigned)vbr(&p->r, 6), i;
            uint64_t first = 0;
            for (i = 0; i < n; i++) { uint64_t v = vbr(&p->r, 6); if (!i) first = v; }
            if (code == 1 && n && !(cur = bi_get(p, (unsigned)first, 1))) return 0;
            continue;
        }
        return 0;
    }
}

static int parse_block(struct parser *p, struct item *blk)
{
    struct abbrev *local = NULL;
    unsigned nlocal = 0, ninfo = 0;
    struct blockinfo *info = blk->code != ~0u ? bi_get(p, blk->code, 0) : NULL;
    int ok = 0;
    if (info) ninfo = info->n;
    while (p->r.pos < p->r.end)
    {
        size_t start = p->r.pos;
        unsigned aid = (unsigned)rd(&p->r, blk->width);
        struct item *it;
        if (p->r.bad) goto done;
        if (aid == 0) { align32(&p->r); blk->end = p->r.pos; ok = 1; goto done; }
        if (aid == 1)
        {
            unsigned bid = (unsigned)vbr(&p->r, 8), width = (unsigned)vbr(&p->r, 4);
            size_t body_end;
            uint64_t nwords;
            align32(&p->r);
            if (!(it = push_item(blk))) goto done;
            it->is_block = 1; it->code = bid; it->width = width; it->start = start;
            it->len_pos = p->r.pos;
            nwords = rd(&p->r, 32);
            it->content_start = p->r.pos;
            body_end = p->r.pos + nwords * 32;
            if (bid == 0 ? !parse_blockinfo(p, it) : !parse_block(p, it)) goto done;
            /* push_item may have moved blk->items; re-fetch */
            it = &blk->items[blk->nitems - 1];
            if (it->end != body_end) goto done;
            continue;
        }
        if (aid == 2)
        {
            struct abbrev a, *n;
            if (!read_abbrev(&p->r, &a)) goto done;
            if (!(n = realloc(local, (nlocal + 1) * sizeof(*n)))) goto done;
            local = n; local[nlocal++] = a;
            if (!(it = push_item(blk))) goto done;
            it->define = 1; it->start = start; it->end = p->r.pos;
            continue;
        }
        if (!(it = push_item(blk))) goto done;
        it->start = start;
        if (aid == 3)
        {
            unsigned n, i;
            it->code = (unsigned)vbr(&p->r, 6);
            n = (unsigned)vbr(&p->r, 6);
            for (i = 0; i < n; i++) if (!push_op(it, vbr(&p->r, 6))) goto done;
        }
        else
        {
            const struct abbrev *a;
            unsigned i, k, first = 1;
            aid -= 4;
            if (aid < ninfo) a = &info->ab[aid];
            else if (aid - ninfo < nlocal) a = &local[aid - ninfo];
            else goto done;
            for (i = 0; i < a->n; i++)
            {
                const struct abbrev_op *op = &a->op[i];
                if (op->kind == OP_ARRAY)
                {
                    unsigned cnt = (unsigned)vbr(&p->r, 6);
                    if (i + 1 >= a->n) goto done;
                    for (k = 0; k < cnt; k++)
                    {
                        uint64_t v = read_scalar(&p->r, &a->op[i + 1]);
                        if (first) { it->code = (unsigned)v; first = 0; }
                        else if (!push_op(it, v)) goto done;
                    }
                    i++;
                    continue;
                }
                if (op->kind == OP_BLOB)
                {
                    /* skipped: blobs only occur in blocks this rewriter copies verbatim */
                    uint64_t len = vbr(&p->r, 6);
                    align32(&p->r);
                    p->r.pos += len * 8;
                    align32(&p->r);
                    if (p->r.pos > p->r.end) goto done;
                    continue;
                }
                {
                    uint64_t v = read_scalar(&p->r, op);
                    if (first) { it->code = (unsigned)v; first = 0; }
                    else if (!push_op(it, v)) goto done;
                }
            }
        }
        it->end = p->r.pos;
        if (p->r.bad) goto done;
    }
    blk->end = p->r.pos;
    ok = 1;
done:
    free(local);
    return ok && !p->r.bad;
}

static void free_items(struct item *blk)
{
    unsigned i;
    for (i = 0; i < blk->nitems; i++)
    {
        free(blk->items[i].ops);
        if (blk->items[i].is_block) free_items(&blk->items[i]);
    }
    free(blk->items);
}

/* ---- module values ------------------------------------------------------------------ */

enum { T_OTHER, T_VOID, T_FLOAT, T_LABEL, T_INT, T_POINTER, T_ARRAY, T_FUNCTION };
struct type { int kind; uint64_t a, b; const uint64_t *ops; unsigned nops; };
enum { V_GLOBAL, V_FUNCTION, V_CONST, V_ARG, V_INST };
struct value { int kind; uint64_t type; int64_t ival; int has_ival; const struct item *rec; /* constants */ };

struct module
{
    struct type *types; unsigned ntypes;
    struct value *values; unsigned nvalues, cap;
    unsigned nfunctions, function_value;
};

static int add_value(struct module *m, int kind, uint64_t type, int64_t ival, int has)
{
    if (m->nvalues == m->cap)
    {
        unsigned cap = m->cap ? m->cap * 2 : 256;
        struct value *v = realloc(m->values, cap * sizeof(*v));
        if (!v) return 0;
        m->values = v; m->cap = cap;
    }
    m->values[m->nvalues].kind = kind; m->values[m->nvalues].type = type;
    m->values[m->nvalues].ival = ival; m->values[m->nvalues].has_ival = has;
    m->values[m->nvalues].rec = NULL;
    m->nvalues++;
    return 1;
}

static int read_types(struct module *m, const struct item *blk)
{
    unsigned i;
    for (i = 0; i < blk->nitems; i++)
    {
        const struct item *it = &blk->items[i];
        struct type *t;
        if (it->is_block || it->define || it->code == 1 || it->code == 19) continue;
        if (!(m->ntypes & 63))
        {
            struct type *n = realloc(m->types, (m->ntypes + 64) * sizeof(*n));
            if (!n) return 0;
            m->types = n;
        }
        t = &m->types[m->ntypes++];
        memset(t, 0, sizeof(*t));
        t->ops = it->ops; t->nops = it->nops;
        switch (it->code)
        {
        case 2: t->kind = T_VOID; break;
        case 3: t->kind = T_FLOAT; break;
        case 5: t->kind = T_LABEL; break;
        case 7: t->kind = T_INT; t->a = it->nops ? it->ops[0] : 0; break;
        case 8: t->kind = T_POINTER; t->a = it->nops ? it->ops[0] : 0; break;
        case 11: t->kind = T_ARRAY; if (it->nops < 2) return 0; t->a = it->ops[0]; t->b = it->ops[1]; break;
        case 21: t->kind = T_FUNCTION; if (it->nops < 2) return 0; t->a = it->ops[0]; t->b = it->ops[1]; break;
        case 4: case 6: case 10: case 12: case 16: case 18: case 20: t->kind = T_OTHER; break;
        default: return 0;
        }
    }
    return 1;
}

static int read_constants(struct module *m, const struct item *blk)
{
    unsigned i;
    uint64_t cur = ~0ull;
    for (i = 0; i < blk->nitems; i++)
    {
        const struct item *it = &blk->items[i];
        if (it->is_block || it->define) continue;
        if (it->code == 1) { if (!it->nops) return 0; cur = it->ops[0]; continue; }
        if (it->code == 4 && it->nops)
        {
            uint64_t v = it->ops[0];
            int64_t s = (v & 1) ? -(int64_t)(v >> 1) : (int64_t)(v >> 1);
            if (!add_value(m, V_CONST, cur, s, 1)) return 0;
        }
        else if (it->code == 2) { if (!add_value(m, V_CONST, cur, 0, 1)) return 0; }
        else if (!add_value(m, V_CONST, cur, 0, 0)) return 0;
        m->values[m->nvalues - 1].rec = it;
    }
    return 1;
}

static int read_module(struct module *m, const struct item *mod)
{
    unsigned i;
    const struct item *consts = NULL;
    for (i = 0; i < mod->nitems; i++)
    {
        const struct item *it = &mod->items[i];
        if (it->is_block)
        {
            if (it->code == 17 && !read_types(m, it)) return 0;
            if (it->code == 11) consts = it;
            continue;
        }
        if (it->define) continue;
        if (it->code == 7) { if (!add_value(m, V_GLOBAL, it->ops[0], 0, 0)) return 0; }
        else if (it->code == 8)
        {
            uint64_t fty;
            if (it->nops < 3 || it->ops[0] >= m->ntypes) return 0;
            fty = it->ops[0];
            if (m->types[fty].kind == T_POINTER) fty = m->types[fty].a;
            if (!add_value(m, V_FUNCTION, fty, 0, 0)) return 0;
            if (!it->ops[2]) { m->nfunctions++; m->function_value = m->nvalues - 1; }
        }
        else if (it->code == 9 || it->code == 14) { if (!add_value(m, V_GLOBAL, 0, 0, 0)) return 0; }
    }
    return !consts || read_constants(m, consts);
}

static int type_ok(const struct module *m, uint64_t t) { return t < m->ntypes; }

static int find_const(const struct module *m, uint64_t type, int64_t v)
{
    unsigned i;
    for (i = 0; i < m->nvalues; i++)
        if (m->values[i].kind == V_CONST && m->values[i].type == type && m->values[i].has_ival
                && m->values[i].ival == v)
            return (int)i;
    return -1;
}

static int int_type(const struct module *m, unsigned width)
{
    unsigned i;
    for (i = 0; i < m->ntypes; i++) if (m->types[i].kind == T_INT && m->types[i].a == width) return (int)i;
    return -1;
}

/* ---- function decoding -------------------------------------------------------------- */

enum { DECLAREBLOCKS = 1, BINOP = 2, CAST = 3, EXTRACTELT = 6, INSERTELT = 7, SHUFFLEVEC = 8, CMP = 9,
       RET = 10, BR = 11, SWITCH = 12, UNREACHABLE = 15, PHI = 16, ALLOCA = 19, LOAD = 20,
       EXTRACTVAL = 26, INSERTVAL = 27, CMP2 = 28, VSELECT = 29, DEBUG_LOC_AGAIN = 33, CALL = 34,
       DEBUG_LOC = 35, FENCE = 36, ATOMICRMW = 38, GEP = 43, STORE = 44, CMPXCHG = 46 };

struct inst
{
    const struct item *rec;
    unsigned refs[64]; unsigned nrefs;
    int signed_refs;            /* phi */
    int64_t value;              /* value id, or -1 */
    uint32_t cur;               /* InstNum when this record was read */
};

struct func
{
    struct inst *insts; unsigned ninsts;
    unsigned first;
};

static uint32_t abs_of(const struct inst *in, unsigned slot)
{
    uint64_t raw = in->rec->ops[slot];
    if (in->signed_refs)
    {
        int64_t r = (raw & 1) ? -(int64_t)(raw >> 1) : (int64_t)(raw >> 1);
        return (uint32_t)((int64_t)in->cur - r);
    }
    return in->cur - (uint32_t)raw;
}

static uint64_t rel_of(int signed_refs, uint32_t absval, uint32_t cur)
{
    if (signed_refs)
    {
        int64_t r = (int64_t)cur - (int64_t)absval;
        return r >= 0 ? (uint64_t)r << 1 : ((uint64_t)(-r) << 1) | 1;
    }
    return (uint32_t)(cur - absval);
}

/* Record a relative operand at slot i; returns the next slot (skipping a forward type). */
static unsigned ref(struct inst *in, unsigned i, int with_type, int *bad)
{
    uint32_t v;
    if (i >= in->rec->nops || in->nrefs == 64) { *bad = 1; return i + 1; }
    v = in->cur - (uint32_t)in->rec->ops[i];
    in->refs[in->nrefs++] = i;
    return i + ((with_type && v >= in->cur) ? 2 : 1);
}

static int decode_function(struct module *m, const struct item *fblk, struct func *f)
{
    const struct type *fty = &m->types[m->values[m->function_value].type];
    unsigned i, k;
    uint32_t inst_num;
    if (fty->kind != T_FUNCTION) return 0;
    for (k = 2; k < fty->nops; k++) if (!add_value(m, V_ARG, fty->ops[k], 0, 0)) return 0;
    for (i = 0; i < fblk->nitems; i++)
    {
        const struct item *it = &fblk->items[i];
        if (!it->is_block) continue;
        if (it->code == 11 && !read_constants(m, it)) return 0;
        if (it->code == 15 || it->code == 18) return 0;     /* function metadata, uselists */
    }
    inst_num = f->first = m->nvalues;
    if (!(f->insts = calloc(fblk->nitems, sizeof(*f->insts)))) return 0;
    for (i = 0; i < fblk->nitems; i++)
    {
        const struct item *it = &fblk->items[i];
        struct inst *in;
        const uint64_t *ops = it->ops;
        unsigned n = it->nops, s;
        int produces = 1, bad = 0;
        if (it->is_block || it->define) continue;
        in = &f->insts[f->ninsts++];
        in->rec = it; in->value = -1; in->cur = inst_num;
        switch (it->code)
        {
        case DECLAREBLOCKS: case DEBUG_LOC: case DEBUG_LOC_AGAIN:
            continue;
        case BINOP: s = ref(in, 0, 1, &bad); ref(in, s, 0, &bad); break;
        case CAST: case LOAD: case EXTRACTVAL: ref(in, 0, 1, &bad); break;
        case CMP: case CMP2: s = ref(in, 0, 1, &bad); ref(in, s, 0, &bad); break;
        case VSELECT: s = ref(in, 0, 1, &bad); s = ref(in, s, 0, &bad); ref(in, s, 1, &bad); break;
        case EXTRACTELT: case INSERTVAL: s = ref(in, 0, 1, &bad); ref(in, s, 1, &bad); break;
        case INSERTELT: s = ref(in, 0, 1, &bad); s = ref(in, s, 0, &bad); ref(in, s, 1, &bad); break;
        case SHUFFLEVEC: s = ref(in, 0, 1, &bad); s = ref(in, s, 0, &bad); ref(in, s, 0, &bad); break;
        case GEP: for (s = 2; s < n && !bad;) s = ref(in, s, 1, &bad); break;
        case STORE: s = ref(in, 0, 1, &bad); ref(in, s, 1, &bad); produces = 0; break;
        case ALLOCA: break;
        case RET: if (n) ref(in, 0, 1, &bad); produces = 0; break;
        case BR: if (n == 3) ref(in, 2, 0, &bad); produces = 0; break;
        case SWITCH: ref(in, 1, 0, &bad); produces = 0; break;
        case UNREACHABLE: case FENCE: produces = 0; break;
        case PHI: in->signed_refs = 1; for (s = 1; s < n; s += 2) { if (in->nrefs == 64) bad = 1; else in->refs[in->nrefs++] = s; } break;
        case ATOMICRMW: s = ref(in, 0, 1, &bad); ref(in, s, 0, &bad); break;
        case CMPXCHG: s = ref(in, 0, 1, &bad); s = ref(in, s, 1, &bad); ref(in, s, 0, &bad); break;
        case CALL:
        {
            uint64_t cc, fty_id = 0;
            uint32_t callee;
            const struct type *ft;
            int explicit_ty;
            if (n < 3) return 0;
            cc = ops[1]; s = 2;
            explicit_ty = (cc >> 15) & 1;
            if (explicit_ty) fty_id = ops[s++];
            if (s >= n) return 0;
            callee = inst_num - (uint32_t)ops[s];
            s = ref(in, s, 1, &bad);
            if (!explicit_ty)
            {
                if (callee >= m->nvalues) return 0;
                fty_id = m->values[callee].type;
                if (type_ok(m, fty_id) && m->types[fty_id].kind == T_POINTER) fty_id = m->types[fty_id].a;
            }
            if (!type_ok(m, fty_id)) return 0;
            ft = &m->types[fty_id];
            if (ft->kind != T_FUNCTION || ft->a) return 0;              /* vararg */
            for (k = 2; k < ft->nops; k++)
            {
                if (!type_ok(m, ft->ops[k])) return 0;
                if (m->types[ft->ops[k]].kind == T_LABEL) s++;
                else s = ref(in, s, 0, &bad);
            }
            if (!type_ok(m, ft->b)) return 0;
            produces = m->types[ft->b].kind != T_VOID;
            break;
        }
        default:
            return 0;
        }
        if (bad) return 0;
        if (produces)
        {
            in->value = inst_num;
            if (!add_value(m, V_INST, 0, 0, 0)) return 0;
            inst_num++;
        }
    }
    return 1;
}

/* ---- rewrite ------------------------------------------------------------------------ */

/*
 * Two rewrites, both at dynamic reads of a local float array (`alloca [N x float]`, read
 * through `getelementptr %a, 0, %idx` with a non-constant %idx), where DXIL leaves an index
 * outside 0..N-1 undefined:
 *
 * SITE_CLAMP (every such read): the GEP index is clamped to 0..N-1, so the array read
 * itself is always defined.
 *
 * SITE_CBREAD (when the array is a copy of a constant buffer): if every slot k is stored
 * exactly once from float B+k of one constant-buffer handle, and the array has no other
 * users, the loaded value is replaced by a direct read of the constant buffer, as
 * dxil-spirv (vkd3d-proton) translates it: float B+idx of the buffer, or 0 outside the
 * buffer's declared size. In range that equals the array; out of range it is what the game
 * gets on Proton. The array read stays (clamped) but its result is no longer used.
 */

enum { SITE_CLAMP, SITE_CBREAD };
#define CLAMP_INSTS 4
#define CBREAD_INSTS 17
#define MAX_SITES 64
#define MAX_NEWCONST 64
#define DXOP_CBUFFER_LOAD_LEGACY 59
#define DXOP_ANNOTATE_HANDLE 216
#define RESOURCE_KIND_CBUFFER 13
#define CBUFFER_MAX_FLOATS 16384             /* 64 KiB, the D3D12 limit */

struct site
{
    unsigned inst; int kind;
    uint32_t idx;                           /* the index value */
    /* clamp: GEP operand slot of the index, constants 0 and N-1 */
    unsigned slot; uint32_t c0, cmax;
    /* cbread: the fill's first cbufferLoadLegacy (copied as a template) and the constants used */
    unsigned call; uint32_t cbase, climit, k0, k1, k2, k3, f0;
};

/* Constants the rewrite needs that the module does not have are appended to the
 * function's constant block; they are numbered first + j, before the instructions. */
struct newconst { uint64_t type; int64_t v; };
struct consts { const struct module *m; uint32_t first; struct newconst nc[MAX_NEWCONST]; unsigned n; int bad; };

static uint32_t get_const(struct consts *c, uint64_t type, int64_t v)
{
    int i = find_const(c->m, type, v);
    unsigned j;
    if (i >= 0) return (uint32_t)i;
    for (j = 0; j < c->n; j++) if (c->nc[j].type == type && c->nc[j].v == v) return c->first + j;
    if (c->n == MAX_NEWCONST) { c->bad = 1; return 0; }
    c->nc[c->n].type = type; c->nc[c->n].v = v;
    return c->first + c->n++;
}

static int const_int(const struct module *m, uint32_t v, int64_t *out)
{
    if (v >= m->nvalues || m->values[v].kind != V_CONST || !m->values[v].has_ival) return 0;
    *out = m->values[v].ival;
    return 1;
}

/* Uses of each value, as a flat list: users of v are use_inst[use_start[v] .. use_start[v+1]),
 * use_slot[] the operand slot within that instruction. */
struct uses { unsigned *start, *inst, *slot; };

static int build_uses(const struct module *m, const struct func *f, struct uses *u)
{
    unsigned i, r, total = 0, *fill;
    u->start = calloc(m->nvalues + 2, sizeof(*u->start));
    if (!u->start) return 0;
    for (i = 0; i < f->ninsts; i++)
        for (r = 0; r < f->insts[i].nrefs; r++)
        {
            uint32_t a = abs_of(&f->insts[i], f->insts[i].refs[r]);
            if (a < m->nvalues) { u->start[a + 1]++; total++; }
        }
    for (i = 0; i < m->nvalues; i++) u->start[i + 1] += u->start[i];
    u->inst = malloc((total + 1) * sizeof(*u->inst));
    u->slot = malloc((total + 1) * sizeof(*u->slot));
    fill = calloc(m->nvalues + 1, sizeof(*fill));
    if (!u->inst || !u->slot || !fill) { free(fill); return 0; }
    for (i = 0; i < f->ninsts; i++)
        for (r = 0; r < f->insts[i].nrefs; r++)
        {
            uint32_t a = abs_of(&f->insts[i], f->insts[i].refs[r]);
            if (a < m->nvalues)
            {
                unsigned k = u->start[a] + fill[a]++;
                u->inst[k] = i; u->slot[k] = f->insts[i].refs[r];
            }
        }
    free(fill);
    return 1;
}

static void free_uses(struct uses *u) { free(u->start); free(u->inst); free(u->slot); }

/* The value a CALL passes as its argument number arg (0-based), or ~0u. CALL refs are the
 * callee followed by the arguments. */
static uint32_t call_arg(const struct inst *c, unsigned arg)
{
    return arg + 1 < c->nrefs ? abs_of(c, c->refs[arg + 1]) : ~0u;
}

static int all_refs_backward(const struct inst *in)
{
    unsigned r;
    for (r = 0; r < in->nrefs; r++) if (abs_of(in, in->refs[r]) >= in->cur) return 0;
    return 1;
}

/* Declared size, in floats, of the constant buffer behind handle h (an SM 6.6 annotateHandle
 * carrying ResourceProperties { CBuffer, size in bytes }); the D3D12 maximum otherwise. */
static int64_t cbuffer_floats(const struct module *m, const struct func *f, const int *v2i, uint32_t h)
{
    const struct inst *c;
    const struct value *props;
    int64_t op, kind, bytes;
    if (h >= m->nvalues || v2i[h] < 0) return CBUFFER_MAX_FLOATS;
    c = &f->insts[v2i[h]];
    if (c->rec->code != CALL || c->nrefs != 4 || !const_int(m, call_arg(c, 0), &op) || op != DXOP_ANNOTATE_HANDLE)
        return CBUFFER_MAX_FLOATS;
    if (call_arg(c, 2) >= m->nvalues) return CBUFFER_MAX_FLOATS;
    props = &m->values[call_arg(c, 2)];
    if (props->kind != V_CONST || !props->rec || props->rec->code != 7 || props->rec->nops != 2)  /* AGGREGATE */
        return CBUFFER_MAX_FLOATS;
    if (!const_int(m, (uint32_t)props->rec->ops[0], &kind) || (kind & 0xff) != RESOURCE_KIND_CBUFFER
            || !const_int(m, (uint32_t)props->rec->ops[1], &bytes) || bytes <= 0 || bytes / 4 > CBUFFER_MAX_FLOATS)
        return CBUFFER_MAX_FLOATS;
    return bytes / 4;
}

/* Is the alloca at instruction ai a copy of consecutive constant-buffer floats? On success
 * returns 1 with the template call (the read filling slot 0), the handle, the first float
 * B and the dynamic loads listed in loads[]. */
static int match_cbuffer_copy(const struct module *m, const struct func *f, const struct uses *u,
                              const int *v2i, unsigned ai, unsigned *call, uint32_t *handle,
                              int64_t *base, unsigned *loads, unsigned *nloads, unsigned max_loads)
{
    const struct inst *a = &f->insts[ai];
    const struct type *t = &m->types[a->rec->ops[0]];
    uint32_t av = (uint32_t)a->value, callee = ~0u, *slot_val;
    unsigned n = (unsigned)t->a, k, j;
    int ok = 0;

    if (n > 4096 || !(slot_val = malloc(n * sizeof(*slot_val)))) return 0;
    for (k = 0; k < n; k++) slot_val[k] = ~0u;
    *nloads = 0;
    for (j = u->start[av]; j < u->start[av + 1]; j++)
    {
        const struct inst *g = &f->insts[u->inst[j]];
        int64_t i0, i1;
        uint32_t gv;
        unsigned q;
        if (g->rec->code != GEP || g->nrefs != 3 || u->slot[j] != g->refs[0]) goto done;
        if (!const_int(m, abs_of(g, g->refs[1]), &i0) || i0) goto done;
        gv = (uint32_t)g->value;
        if (const_int(m, abs_of(g, g->refs[2]), &i1))
        {
            if (i1 < 0 || i1 >= n) goto done;
            for (q = u->start[gv]; q < u->start[gv + 1]; q++)
            {
                const struct inst *s = &f->insts[u->inst[q]];
                if (s->rec->code == LOAD) continue;
                if (s->rec->code != STORE || u->slot[q] != s->refs[0] || slot_val[i1] != ~0u) goto done;
                slot_val[i1] = abs_of(s, s->refs[1]);
            }
        }
        else
        {
            for (q = u->start[gv]; q < u->start[gv + 1]; q++)
            {
                if (f->insts[u->inst[q]].rec->code != LOAD || *nloads == max_loads) goto done;
                loads[(*nloads)++] = u->inst[q];
            }
        }
    }
    if (!*nloads) goto done;
    for (k = 0; k < n; k++)
    {
        const struct inst *e, *c;
        int64_t op, row;
        if (slot_val[k] >= m->nvalues || v2i[slot_val[k]] < 0) goto done;
        e = &f->insts[v2i[slot_val[k]]];
        if (e->rec->code != EXTRACTVAL || e->nrefs != 1 || e->rec->nops != 2 || !all_refs_backward(e)) goto done;
        if (v2i[abs_of(e, e->refs[0])] < 0) goto done;
        c = &f->insts[v2i[abs_of(e, e->refs[0])]];
        if (c->rec->code != CALL || c->nrefs != 4 || !all_refs_backward(c)) goto done;
        if (!const_int(m, call_arg(c, 0), &op) || op != DXOP_CBUFFER_LOAD_LEGACY) goto done;
        if (!const_int(m, call_arg(c, 2), &row) || row < 0 || e->rec->ops[1] > 3) goto done;
        if (!k)
        {
            callee = abs_of(c, c->refs[0]); *handle = call_arg(c, 1);
            *call = (unsigned)(c - f->insts);
            *base = row * 4 + (int64_t)e->rec->ops[1];
        }
        else if (abs_of(c, c->refs[0]) != callee || call_arg(c, 1) != *handle
                 || row * 4 + (int64_t)e->rec->ops[1] != *base + k)
            goto done;
    }
    ok = 1;
done:
    free(slot_val);
    return ok;
}

static int cmp_site(const void *x, const void *y)
{
    const struct site *a = x, *b = y;
    return a->inst < b->inst ? -1 : a->inst > b->inst;
}

static unsigned find_sites(const struct module *m, const struct func *f, struct consts *cs,
                           struct site *sites, unsigned max, unsigned *ncb)
{
    unsigned i, n = 0;
    int i32 = int_type(m, 32);
    int *v2i = NULL;
    struct uses u = { 0 };

    *ncb = 0;
    if (i32 < 0) return 0;
    if (!(v2i = malloc((m->nvalues + 1) * sizeof(*v2i))) || !build_uses(m, f, &u)) goto fail;
    for (i = 0; i <= m->nvalues; i++) v2i[i] = -1;
    for (i = 0; i < f->ninsts; i++) if (f->insts[i].value >= 0) v2i[f->insts[i].value] = (int)i;

    for (i = 0; i < f->ninsts; i++)
    {
        const struct inst *g = &f->insts[i];
        const struct inst *a;
        const struct type *t;
        uint32_t base, idx;
        if (g->rec->code != GEP || g->nrefs != 3) continue;
        base = abs_of(g, g->refs[0]);
        idx = abs_of(g, g->refs[2]);
        if (idx >= m->nvalues || m->values[idx].kind == V_CONST) continue;
        if (base >= m->nvalues || v2i[base] < 0) continue;
        a = &f->insts[v2i[base]];
        if (a->rec->code != ALLOCA || !a->rec->nops || !type_ok(m, a->rec->ops[0])) continue;
        t = &m->types[a->rec->ops[0]];
        if (t->kind != T_ARRAY || !type_ok(m, t->b) || m->types[t->b].kind != T_FLOAT || !t->a) continue;
        if (n == max) goto fail;
        sites[n].inst = i; sites[n].kind = SITE_CLAMP; sites[n].idx = idx; sites[n].slot = g->refs[2];
        sites[n].c0 = get_const(cs, (uint64_t)i32, 0);
        sites[n].cmax = get_const(cs, (uint64_t)i32, (int64_t)t->a - 1);
        n++;
    }

    for (i = 0; i < f->ninsts; i++)
    {
        const struct inst *a = &f->insts[i];
        const struct type *t;
        unsigned loads[16], nloads, call = 0, l;
        uint32_t handle = ~0u;
        int64_t base = 0, limit;
        if (a->rec->code != ALLOCA || !a->rec->nops || !type_ok(m, a->rec->ops[0])) continue;
        t = &m->types[a->rec->ops[0]];
        if (t->kind != T_ARRAY || !type_ok(m, t->b) || m->types[t->b].kind != T_FLOAT || !t->a) continue;
        if (!match_cbuffer_copy(m, f, &u, v2i, i, &call, &handle, &base, loads, &nloads, 16)) continue;
        limit = cbuffer_floats(m, f, v2i, handle);
        for (l = 0; l < nloads; l++)
        {
            const struct inst *ld = &f->insts[loads[l]];
            const struct inst *g = &f->insts[v2i[abs_of(ld, ld->refs[0])]];
            struct site *s;
            if (n == max) goto fail;
            s = &sites[n++];
            s->inst = loads[l]; s->kind = SITE_CBREAD; s->idx = abs_of(g, g->refs[2]); s->call = call;
            s->cbase = get_const(cs, (uint64_t)i32, base);
            s->climit = get_const(cs, (uint64_t)i32, limit);
            s->k0 = get_const(cs, (uint64_t)i32, 0); s->k1 = get_const(cs, (uint64_t)i32, 1);
            s->k2 = get_const(cs, (uint64_t)i32, 2); s->k3 = get_const(cs, (uint64_t)i32, 3);
            s->f0 = get_const(cs, t->b, 0);                 /* float 0.0 (a NULL constant) */
            (*ncb)++;
        }
    }
    if (cs->bad) goto fail;
    qsort(sites, n, sizeof(*sites), cmp_site);
    for (i = 1; i < n; i++) if (sites[i].inst == sites[i - 1].inst) goto fail;
    free(v2i); free_uses(&u);
    return n;
fail:
    *ncb = 0;
    free(v2i); free_uses(&u);
    return 0;
}

static unsigned site_insts(const struct site *s) { return s->kind == SITE_CLAMP ? CLAMP_INSTS : CBREAD_INSTS; }

/* The function's constant block plus the new constants (or a new block holding them). */
static void emit_constants(struct writer *w, const struct item *blk, const struct consts *cs,
                           const uint8_t *data, unsigned parent_width)
{
    unsigned width = blk ? blk->width : 4, j;
    size_t lp, st, end;
    uint64_t cur_type = ~0ull;
    wr(w, 1, parent_width); wvbr(w, 11, 8); wvbr(w, width, 4); walign32(w);
    lp = w->pos; wr(w, 0, 32); st = w->pos;
    if (blk)
    {
        end = blk->nitems ? blk->items[blk->nitems - 1].end : blk->content_start;
        wcopy(w, data, blk->content_start, end);
    }
    for (j = 0; j < cs->n; j++)
    {
        uint64_t op;
        if (cs->nc[j].type != cur_type) { op = cs->nc[j].type; wrecord(w, width, 1, &op, 1); cur_type = op; }
        if (cs->m->types[cs->nc[j].type].kind == T_FLOAT)
            wrecord(w, width, 2, NULL, 0);                                      /* NULL: 0.0 */
        else
        {
            int64_t v = cs->nc[j].v;
            op = v < 0 ? ((uint64_t)(-v) << 1) | 1 : (uint64_t)v << 1;
            wrecord(w, width, 4, &op, 1);                                       /* INTEGER */
        }
    }
    wr(w, 0, width); walign32(w);
    wpatch32(w, lp, (uint32_t)((w->pos - st) / 32));
}

static void emit_clamp(struct writer *w, unsigned width, const struct site *st, uint32_t idx, uint32_t *cur)
{
    uint32_t c = *cur, c1 = c, s1 = c + 1, c2 = c + 2;
    uint64_t o[3];
    o[0] = c - idx; o[1] = c - st->c0; o[2] = 40;                                /* icmp slt idx, 0 */
    wrecord(w, width, CMP2, o, 3); c++;
    o[0] = c - st->c0; o[1] = c - idx; o[2] = c - c1;                           /* select c1, 0, idx */
    wrecord(w, width, VSELECT, o, 3); c++;
    o[0] = c - s1; o[1] = c - st->cmax; o[2] = 38;                              /* icmp sgt s1, N-1 */
    wrecord(w, width, CMP2, o, 3); c++;
    o[0] = c - st->cmax; o[1] = c - s1; o[2] = c - c2;                          /* select c2, N-1, s1 */
    wrecord(w, width, VSELECT, o, 3); c++;
    *cur = c;
}

#define BIN(a, b, opc) do { o[0] = c - (a); o[1] = c - (b); o[2] = (opc); wrecord(w, width, BINOP, o, 3); c++; } while (0)
#define ICMP(a, b, pred) do { o[0] = c - (a); o[1] = c - (b); o[2] = (pred); wrecord(w, width, CMP2, o, 3); c++; } while (0)
#define SEL(cond, t, e) do { o[0] = c - (t); o[1] = c - (e); o[2] = c - (cond); wrecord(w, width, VSELECT, o, 3); c++; } while (0)

static int emit_cbread(struct writer *w, unsigned width, const struct func *f, const struct site *st,
                       const uint32_t *remap, uint32_t nvalues, uint32_t idx, uint32_t *cur)
{
    const struct inst *tc = &f->insts[st->call];
    uint32_t c = *cur, off, okv, offc, row, comp, ret, v[4], e0, e1, e2, s23, s1;
    uint64_t o[256];
    unsigned r, k;

    off = c;  BIN(idx, st->cbase, 0);                       /* add idx, B */
    okv = c;  ICMP(off, st->climit, 36);                    /* icmp ult off, size: negative is out too */
    offc = c; SEL(okv, off, st->k0);                        /* keep the buffer read in range */
    row = c;  BIN(offc, st->k2, 8);                         /* lshr: 16-byte row */
    comp = c; BIN(offc, st->k3, 10);                        /* and: float within the row */
    if (tc->rec->nops > 256) return 0;
    memcpy(o, tc->rec->ops, tc->rec->nops * sizeof(*o));    /* cbufferLoadLegacy(59, handle, row) */
    for (r = 0; r < tc->nrefs; r++)
    {
        uint32_t a = abs_of(tc, tc->refs[r]);
        o[tc->refs[r]] = c - (r == 3 ? row : (a < nvalues ? remap[a] : a));
    }
    ret = c; wrecord(w, width, CALL, o, tc->rec->nops); c++;
    for (k = 0; k < 4; k++) { v[k] = c; o[0] = c - ret; o[1] = k; wrecord(w, width, EXTRACTVAL, o, 2); c++; }
    e0 = c; ICMP(comp, st->k0, 32);
    e1 = c; ICMP(comp, st->k1, 32);
    e2 = c; ICMP(comp, st->k2, 32);
    s23 = c; SEL(e2, v[2], v[3]);
    s1 = c;  SEL(e1, v[1], s23);
    k = c;   SEL(e0, v[0], s1);
    SEL(okv, k, st->f0);                                    /* 0 outside the buffer */
    *cur = c;
    return 1;
}

#undef BIN
#undef ICMP
#undef SEL

static int emit_function(struct module *m, const struct item *fblk, const struct func *f,
                         const struct site *sites, unsigned nsites, const struct consts *cs,
                         struct writer *w, const uint8_t *data, unsigned parent_width)
{
    unsigned width = fblk->width, i, k, s;
    size_t len_pos, body_start;
    uint32_t *remap, cur, next;
    unsigned *inst_map, new_index = 0, old_index = 0;
    int emitted = 0, ok = 0, have_consts = 0;

    if (!(remap = calloc(m->nvalues + 1, sizeof(*remap)))) return 0;
    if (!(inst_map = calloc(f->ninsts + 1, sizeof(*inst_map)))) { free(remap); return 0; }
    for (i = 0; i < m->nvalues; i++) remap[i] = i;
    /* new numbering: new constants first, then the inserted instructions before each site;
     * a cbuffer read's result takes over the uses of the array load it replaces */
    next = f->first + cs->n;
    for (i = 0, s = 0; i < f->ninsts; i++)
    {
        uint32_t replaced = ~0u;
        if (s < nsites && sites[s].inst == i)
        {
            next += site_insts(&sites[s]);
            if (sites[s].kind == SITE_CBREAD) replaced = next - 1;
            s++;
        }
        if (f->insts[i].value >= 0) { remap[f->insts[i].value] = replaced != ~0u ? replaced : next; next++; }
    }

    wr(w, 1, parent_width); wvbr(w, 12, 8); wvbr(w, width, 4); walign32(w);
    len_pos = w->pos; wr(w, 0, 32); body_start = w->pos;

    for (k = 0; k < fblk->nitems; k++)
        if (fblk->items[k].is_block && fblk->items[k].code == 11) have_consts = 1;
    for (k = 0; k < fblk->nitems; k++)
    {
        const struct item *it = &fblk->items[k];
        if (it->is_block)
        {
            if (it->code == 14 || it->code == 16) continue;          /* written after */
            if (it->code == 11) { emit_constants(w, it, cs, data, width); continue; }
            wr(w, 1, width); wvbr(w, it->code, 8); wvbr(w, it->width, 4); walign32(w);
            wr(w, (it->end - it->content_start) / 32, 32);
            wcopy(w, data, it->content_start, it->end);
            continue;
        }
        if (it->define) continue;
        if (it->code == DECLAREBLOCKS) { wrecord(w, width, it->code, it->ops, it->nops); continue; }
        if (emitted) continue;
        emitted = 1;
        if (!have_consts && cs->n) emit_constants(w, NULL, cs, data, width);
        cur = f->first + cs->n;
        for (i = 0, s = 0; i < f->ninsts; i++)
        {
            const struct inst *in = &f->insts[i];
            uint64_t ops[256];
            unsigned n = in->rec->nops, r;
            int clamp = 0;
            uint32_t s2 = 0;
            if (in->rec->code == DECLAREBLOCKS) continue;
            if (s < nsites && sites[s].inst == i)
            {
                uint32_t idx = sites[s].idx < m->nvalues ? remap[sites[s].idx] : sites[s].idx;
                if (sites[s].kind == SITE_CLAMP) { emit_clamp(w, width, &sites[s], idx, &cur); s2 = cur - 1; clamp = 1; }
                else if (!emit_cbread(w, width, f, &sites[s], remap, m->nvalues, idx, &cur)) goto out;
                new_index += site_insts(&sites[s]);
            }
            if (n > 256) goto out;
            memcpy(ops, in->rec->ops, n * sizeof(*ops));
            if (in->rec->code != DEBUG_LOC && in->rec->code != DEBUG_LOC_AGAIN)
            {
                for (r = 0; r < in->nrefs; r++)
                {
                    uint32_t a = abs_of(in, in->refs[r]);
                    uint32_t na = a < m->nvalues ? remap[a] : a;
                    ops[in->refs[r]] = rel_of(in->signed_refs, na, cur);
                }
                if (clamp) ops[sites[s].slot] = (uint32_t)(cur - s2);
            }
            if (s < nsites && sites[s].inst == i) s++;
            wrecord(w, width, in->rec->code, ops, n);
            if (in->rec->code == DEBUG_LOC || in->rec->code == DEBUG_LOC_AGAIN) continue;
            inst_map[old_index++] = new_index++;
            if (in->value >= 0) cur++;
        }
    }
    for (k = 0; k < fblk->nitems; k++)
    {
        const struct item *it = &fblk->items[k];
        unsigned bw = it->code == 16 ? 3 : 4;
        size_t lp, st;
        if (!it->is_block || (it->code != 14 && it->code != 16)) continue;
        wr(w, 1, width); wvbr(w, it->code, 8); wvbr(w, bw, 4); walign32(w);
        lp = w->pos; wr(w, 0, 32); st = w->pos;
        for (i = 0; i < it->nitems; i++)
        {
            const struct item *rc = &it->items[i];
            uint64_t ops[256];
            if (rc->is_block || rc->define) continue;
            if (rc->nops > 256) goto out;
            memcpy(ops, rc->ops, rc->nops * sizeof(*ops));
            if (it->code == 16 && (rc->nops & 1))
            {
                if (ops[0] >= old_index) goto out;
                ops[0] = inst_map[ops[0]];
            }
            if (it->code == 14 && rc->code == 1 && rc->nops && ops[0] < m->nvalues) ops[0] = remap[ops[0]];
            wrecord(w, bw, rc->code, ops, rc->nops);
        }
        wr(w, 0, bw); walign32(w);
        wpatch32(w, lp, (uint32_t)((w->pos - st) / 32));
    }
    wr(w, 0, width); walign32(w);
    wpatch32(w, len_pos, (uint32_t)((w->pos - body_start) / 32));
    ok = !w->bad;
out:
    free(remap); free(inst_map);
    return ok;
}

/* Rewrites a raw DXIL bitcode buffer. Returns the number of sites rewritten (0: unchanged);
 * *ncb is how many of them read the constant buffer directly. */
static unsigned rewrite_bitcode(const uint8_t *bc, size_t size, uint8_t **out, size_t *out_size, unsigned *ncb)
{
    struct parser p;
    struct item top, *mod = NULL, *fblk = NULL;
    struct module m;
    struct func f;
    struct site sites[MAX_SITES];
    struct consts *cs = NULL;
    unsigned i, nsites = 0, nfb = 0;
    struct writer w = { 0 };
    size_t mod_len_pos;

    *out = NULL; *ncb = 0;
    if (size < 8 || memcmp(bc, "BC\xc0\xde", 4)) return 0;
    memset(&p, 0, sizeof(p)); memset(&top, 0, sizeof(top)); memset(&m, 0, sizeof(m)); memset(&f, 0, sizeof(f));
    p.r.data = bc; p.r.end = size * 8; p.r.pos = 32;
    top.code = ~0u; top.width = 2;
    if (!parse_block(&p, &top)) goto done;
    for (i = 0; i < top.nitems; i++) if (top.items[i].is_block && top.items[i].code == 8) mod = &top.items[i];
    if (!mod || !read_module(&m, mod) || m.nfunctions != 1) goto done;
    for (i = 0; i < mod->nitems; i++) if (mod->items[i].is_block && mod->items[i].code == 12) { fblk = &mod->items[i]; nfb++; }
    if (nfb != 1 || !decode_function(&m, fblk, &f)) goto done;
    if (!(cs = calloc(1, sizeof(*cs)))) goto done;
    cs->m = &m; cs->first = f.first;
    if (!(nsites = find_sites(&m, &f, cs, sites, MAX_SITES, ncb))) goto done;

    wcopy(&w, bc, 0, mod->len_pos);
    mod_len_pos = w.pos; wr(&w, 0, 32);
    wcopy(&w, bc, mod->content_start, fblk->start);
    if (!emit_function(&m, fblk, &f, sites, nsites, cs, &w, bc, mod->width)) { nsites = 0; goto done; }
    wcopy(&w, bc, fblk->end, mod->end);
    wpatch32(&w, mod_len_pos, (uint32_t)((w.pos - mod->content_start) / 32));
    wcopy(&w, bc, mod->end, size * 8);
    if (w.bad || (w.pos & 31)) { nsites = 0; goto done; }
    *out = w.buf; *out_size = w.pos / 8; w.buf = NULL;
done:
    if (!*out) *ncb = 0;
    free(cs);
    free(w.buf);
    free(f.insts);
    free(m.types); free(m.values);
    for (i = 0; i < p.nbi; i++) free(p.bi[i].ab);
    free_items(&top);
    return *out ? nsites : 0;
}

/* ---- DXBC container ----------------------------------------------------------------- */

static const uint32_t md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const unsigned md5_s[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
    5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
    6, 10, 15, 21, 6, 10, 15, 21 };

static void md5_block(uint32_t st[4], const uint8_t blk[64])
{
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], mw[16], fv, t;
    unsigned i, g;
    for (i = 0; i < 16; i++) memcpy(&mw[i], blk + 4 * i, 4);
    for (i = 0; i < 64; i++)
    {
        if (i < 16) { fv = (b & c) | (~b & d); g = i; }
        else if (i < 32) { fv = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
        else if (i < 48) { fv = b ^ c ^ d; g = (3 * i + 5) % 16; }
        else { fv = c ^ (b | ~d); g = (7 * i) % 16; }
        fv += a + md5_k[i] + mw[g];
        a = d; d = c; c = b;
        t = (fv << md5_s[i]) | (fv >> (32 - md5_s[i]));
        b += t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d;
}

/* DXBC checksum: MD5 with the bit count first and (bits >> 2) | 1 last in the final block. */
static void dxbc_checksum(const uint8_t *data, size_t size, uint8_t out[16])
{
    uint32_t st[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, bits = (uint32_t)(size * 8), tail_bits;
    uint8_t last[64];
    size_t full = size - size % 64, off, count;
    for (off = 0; off < full; off += 64) md5_block(st, data + off);
    count = size - full;
    memset(last, 0, sizeof(last));
    if (64 - (count + 1) < 8)
    {
        memcpy(last, data + full, count); last[count] = 0x80;
        md5_block(st, last);
        memset(last, 0, sizeof(last));
    }
    else
    {
        memcpy(last + 4, data + full, count); last[4 + count] = 0x80;
    }
    memcpy(last, &bits, 4);
    tail_bits = (bits >> 2) | 1;
    memcpy(last + 60, &tail_bits, 4);
    md5_block(st, last);
    memcpy(out, st, 16);
}

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

unsigned dxil_rewrite_container(const void *blob, size_t size, void **out, size_t *out_size, unsigned *cb_reads)
{
    const uint8_t *in = blob;
    uint32_t count, i, dxil = ~0u, total;
    const uint8_t *prog;
    uint32_t prog_size, bc_off, bc_size;
    uint8_t *bc_new = NULL, *res;
    size_t bc_new_size = 0, pos;
    unsigned n, ncb = 0;

    *out = NULL; *out_size = 0;
    if (cb_reads) *cb_reads = 0;
    if (size < 32 || memcmp(in, "DXBC", 4) || rd32(in + 24) != size) return 0;
    count = rd32(in + 28);
    if (count > 64 || 32 + 4 * (size_t)count > size) return 0;
    for (i = 0; i < count; i++)
    {
        uint32_t off = rd32(in + 32 + 4 * i);
        if ((size_t)off + 8 > size || (size_t)off + 8 + rd32(in + off + 4) > size) return 0;
        if (!memcmp(in + off, "DXIL", 4)) dxil = i;
    }
    if (dxil == ~0u) return 0;
    prog = in + rd32(in + 32 + 4 * dxil) + 8;
    prog_size = rd32(in + rd32(in + 32 + 4 * dxil) + 4);
    if (prog_size < 24 || rd32(prog + 8) != 0x4C495844) return 0;
    bc_off = rd32(prog + 16); bc_size = rd32(prog + 20);
    if (bc_off != 16 || 8 + (size_t)bc_off + bc_size > prog_size) return 0;
    if (!(n = rewrite_bitcode(prog + 24, bc_size, &bc_new, &bc_new_size, &ncb))) return 0;

    {
        size_t new_prog = 24 + ((bc_new_size + 3) & ~(size_t)3);
        total = 32 + 4 * count;
        for (i = 0; i < count; i++)
            total += 8 + (i == dxil ? (uint32_t)new_prog : rd32(in + rd32(in + 32 + 4 * i) + 4));
        if (!(res = calloc(1, total))) { free(bc_new); return 0; }
        memcpy(res, "DXBC", 4);
        memcpy(res + 20, in + 20, 4);
        put32(res + 24, total); put32(res + 28, count);
        pos = 32 + 4 * count;
        for (i = 0; i < count; i++)
        {
            const uint8_t *part = in + rd32(in + 32 + 4 * i);
            put32(res + 32 + 4 * i, (uint32_t)pos);
            memcpy(res + pos, part, 4);
            if (i == dxil)
            {
                put32(res + pos + 4, (uint32_t)new_prog);
                memcpy(res + pos + 8, prog, 4);                         /* program version */
                put32(res + pos + 12, (uint32_t)(new_prog / 4));
                memcpy(res + pos + 16, prog + 8, 8);                    /* 'DXIL', dxil version */
                put32(res + pos + 24, 16);
                put32(res + pos + 28, (uint32_t)bc_new_size);
                memcpy(res + pos + 32, bc_new, bc_new_size);
                pos += 8 + new_prog;
            }
            else
            {
                uint32_t psz = rd32(part + 4);
                memcpy(res + pos + 4, part + 4, 4 + psz);
                pos += 8 + psz;
            }
        }
    }
    free(bc_new);
    dxbc_checksum(res + 20, total - 20, res + 4);
    *out = res; *out_size = total;
    if (cb_reads) *cb_reads = ncb;
    return n;
}
