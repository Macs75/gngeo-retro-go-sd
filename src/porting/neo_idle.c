/*
 * neo_idle.c -- generic 68000 idle-loop skip: the analysis and the pass
 * runner (cold, .neo_flash).
 *
 * Neo Geo games finish a frame's work early and then spin until the VBL
 * interrupt: the watchdog kick (`MOVE.B D0,$300001 / BRA`), a RAM flag
 * poll (`TST.B flag / BEQ`), the UniBIOS wait (`TST.B $10FE8C / BNE`), or
 * a poll that also steps something each pass (a counter, neodrift's random
 * number). This core runs the 68000 for the whole frame in one slice and
 * raises the VBL after it (neo_run_frame): nothing outside the 68000
 * changes what such a loop sees before the slice ends.
 *
 *  1. ARM (neo_idle_slice_end, once per slice): the slice almost always
 *     ends inside the wait loop. From its PC, decode forward to the branch
 *     that closes the loop: its target is the loop's head.
 *  2. TRACE (neo_idle_trace, the first time the head comes round): run one
 *     pass from the head on a SHADOW of the machine (registers, flags, a
 *     byte log of the stores) with this file's own interpreter, following
 *     the branches the state takes, through JSR/BSR and RTS, back to the
 *     head with the stack where it was. The pass is recorded as a list of
 *     decoded operations, each branch with the way it went, and its length
 *     in Musashi's cycles. Anything outside the supported subset (below),
 *     any store outside work RAM but the watchdog, any read outside direct-
 *     mapped memory (an I/O register can change within the frame) rejects.
 *  3. SKIP (neo_idle_hit in m68kcpu.c, then neo_idle_run): when the head
 *     comes round exactly one pass after the last time (the PC went round
 *     the loop), with N whole passes left in the slice, N-1 are taken here:
 *     a pass is run on the shadow; if it leaves registers, flags and memory
 *     as they are, every later pass does too, and all N-1 are consumed at
 *     once (the cycles only). Otherwise the passes run here, natively, one
 *     after another, each committed only if it took every branch the way
 *     the trace did (else Musashi takes over at the head, in that state).
 *     The last pass and the slice's tail always run in Musashi.
 * The machine ends the slice exactly where Musashi alone would have: the
 * operations reproduce Musashi's handlers (results, the flags' raw values,
 * cycles: the opcode table plus the not-taken branch and shift-count
 * extras). Verified by the REC/hash lines of replays, skip on and off.
 *
 * The subset: NOP, Bcc/BRA/BSR, JSR (abs, d16(PC), (An), d16(An)), RTS,
 * TST, CLR, MOVE (not to An), ORI/ANDI/SUBI/ADDI/EORI/CMPI, BTST, ADDQ/
 * SUBQ (not to An), OR/AND/SUB/ADD/EOR/CMP with a data register, and
 * ASL/ASR/LSL/LSR/ROL/ROR.W by an immediate count. Effective addresses Dn,
 * An (read), (An), d16(An), abs.W/.L, d16(PC), #imm, and -(A7)/(A7)+:
 * nothing writes A0-A6, so their addresses hold for the whole pass.
 *
 * NEO_IDLE_SKIP=0 (make) leaves it out; host: NEO_NOIDLE=1 disarms,
 * NEO_IDLEBUG=1 charges each skipped pass one cycle short (the control,
 * which must change the hashes), NEO_IDLE_LOG=1|2|3 logs arms and rejects,
 * each skip, each slice end.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "memory.h"
#undef READ_WORD                 /* memory.h's; m68k/macros.h has its own */
#undef WRITE_WORD
#undef READ_BYTE
#undef WRITE_BYTE
#include "m68k/m68k.h"
#include "gw_malloc.h"
#include "neo_idle.h"

#if NEO_IDLE_SKIP

unsigned int neo_idle_head = NEO_IDLE_NONE;
unsigned int neo_idle_lap, neo_idle_last;
unsigned int neo_idle_skipped, neo_idle_skips, neo_idle_ran;
unsigned int neo_idle_bug;
unsigned int neo_idle_arms, neo_idle_rejects;
unsigned int neo_idle_log;

extern unsigned char *m68ki_cycles;          /* Musashi's per-opcode cycles */
static unsigned int rej[8], rej_n;
static int host_off = -1;
static unsigned int trace_fails;

/* ------------------------------------------------------------------ */
/* memory                                                              */

static const cpu_memory_map *map_of(unsigned int a)
{
    return &m68k.memory_map[(a >> 16) & 0xff];
}

/* direct-mapped memory: no handler, so a read has no side effect and the
 * value changes only by a 68000 write. The program ROM's first 64 KiB has
 * handlers only for the vector overlay at $000000-$00007F (m68k_interf.c,
 * neo_read16_cpu0): above it, it reads the ROM at base like the rest. */
static int direct_read(unsigned int a)
{
    a &= 0xffffff;
    const cpu_memory_map *m = map_of(a);
    if (a < 0x10000)
        return a >= 0x80 && m->base;
    return m->base && !m->read8 && !m->read16;
}

static int ram_write(unsigned int a)
{
    a &= 0xffffff;
    if (a < 0x100000 || a > 0x1fffff)
        return 0;
    const cpu_memory_map *m = map_of(a);
    return m->base && !m->write8 && !m->write16;
}

static int fetch16(unsigned int a, unsigned int *w)
{
    a &= 0xffffff;
    if ((a & 1) || !direct_read(a))
        return 0;
    *w = *(const uint16_t *)(map_of(a)->base + (a & 0xffff));
    return 1;
}

static int sext16(unsigned int w) { return (int)(int16_t)(uint16_t)w; }

/* ------------------------------------------------------------------ */
/* the operations                                                      */

enum { T_NONE, T_D, T_A, T_IMM, T_ABS, T_AIND, T_PUSH, T_POP };
typedef struct { uint8_t t, reg; uint32_t v; } opd_t;

enum { K_NOP, K_BCC, K_JSR, K_RTS, K_TST, K_CMP, K_ADD, K_SUB, K_AND, K_OR,
       K_EOR, K_MOVE, K_CLR, K_BTST, K_SHIFT, K_BMOD, K_DBCC };
enum { BM_CHG, BM_CLR, BM_SET };          /* K_BMOD: BCHG / BCLR / BSET (sh) */
/* K_DBCC's way (taken): the condition held and it fell through, the count
 * went on and it branched, or the count expired and it fell through */
enum { DB_COND, DB_LOOP, DB_EXPIRED };
enum { SH_ASL, SH_ASR, SH_LSL, SH_LSR, SH_ROL, SH_ROR };

typedef struct {
    uint8_t k, sz, cc, taken;    /* sz 1/2/4; Bcc/DBcc: condition, the way the trace went */
    uint8_t sh, cnt, x;          /* shift kind and count; x: ADD/SUB set X */
    uint8_t need;                /* the flags (F_*) a later branch reads: liveness() */
    uint16_t cyc;                /* Musashi's cycles for this instruction, as traced */
    uint32_t pc, ret;            /* JSR/BSR: the pushed return; RTS: the expected pop */
    opd_t s, d;
} iop_t;

#define MAXOPS 24
static iop_t *ops;                   /* the traced pass (idle_mem) */
static int nops;
static unsigned int trace_sp;

/* the shadow machine: registers, flags (Musashi's raw values), byte stores */
typedef struct { uint32_t r[16], x, n, z, v, c; } regs_t;
#define MAXW 64
static uint32_t *w_a;                /* the shadow's byte stores (idle_mem) */
static uint8_t *w_v;
static int w_n, w_dog;

static int rd8(unsigned int a, uint32_t *v)
{
    a &= 0xffffff;
    for (int i = w_n - 1; i >= 0; i--)
        if (w_a[i] == a) { *v = w_v[i]; return 1; }
    if (!direct_read(a))
        return 0;
    *v = map_of(a)->base[(a ^ 1) & 0xffff];
    return 1;
}

static int rdm(unsigned int a, unsigned int sz, uint32_t *v)
{
    uint32_t b, r = 0;
    if (sz > 1 && (a & 1))
        return 0;
    for (unsigned int i = 0; i < sz; i++) {
        if (!rd8(a + i, &b)) return 0;
        r = (r << 8) | b;
    }
    *v = r;
    return 1;
}

static int wrm(unsigned int a, unsigned int sz, uint32_t v)
{
    a &= 0xffffff;
    if (sz > 1 && (a & 1))
        return 0;
    if ((a & ~1u) == 0x300000) {               /* the watchdog kick */
        w_dog = 1;
        return 1;
    }
    for (unsigned int i = 0; i < sz; i++) {
        unsigned int b = a + i;
        if (!ram_write(b) || w_n == MAXW) return 0;
        w_a[w_n] = b;
        w_v[w_n] = (uint8_t)(v >> (8 * (sz - 1 - i)));
        w_n++;
    }
    return 1;
}

static inline __attribute__((always_inline)) uint32_t mask_of(unsigned int sz) { return sz == 1 ? 0xff : sz == 2 ? 0xffff : 0xffffffffu; }

/* an operand's value; for T_PUSH/T_POP the stack pointer moves here */
static int get(regs_t *r, const opd_t *o, unsigned int sz, uint32_t *v)
{
    unsigned int step = (sz == 1) ? 2 : sz;    /* A7 byte push/pop keeps it even */
    switch (o->t) {
    case T_D: *v = r->r[o->reg] & mask_of(sz); return 1;
    case T_A: *v = r->r[8 + o->reg] & mask_of(sz); return 1;
    case T_IMM: *v = o->v & mask_of(sz); return 1;
    case T_ABS: return rdm(o->v, sz, v);
    case T_AIND: return rdm(r->r[8 + o->reg] + o->v, sz, v);
    case T_POP:
        if (!rdm(r->r[15], sz, v)) return 0;
        r->r[15] += step;
        return 1;
    }
    return 0;
}

/* the destination's address for a read-modify-write */
static int addr_of(const regs_t *r, const opd_t *o, unsigned int *a)
{
    if (o->t == T_ABS) { *a = o->v; return 1; }
    if (o->t == T_AIND) { *a = r->r[8 + o->reg] + o->v; return 1; }
    return 0;
}

static int put(regs_t *r, const opd_t *o, unsigned int sz, uint32_t v)
{
    unsigned int step = (sz == 1) ? 2 : sz, a;
    uint32_t m = mask_of(sz);
    switch (o->t) {
    case T_D: r->r[o->reg] = (r->r[o->reg] & ~m) | (v & m); return 1;
    case T_ABS: case T_AIND:
        addr_of(r, o, &a);
        return wrm(a, sz, v);
    case T_PUSH:
        r->r[15] -= step;
        return wrm(r->r[15], sz, v);
    }
    return 0;
}

/* Musashi's flag macros (m68kcpu.h, 32-bit int) */
static inline __attribute__((always_inline)) uint32_t nflag(uint32_t res, unsigned int sz) { return sz == 1 ? res : sz == 2 ? res >> 8 : res >> 24; }
static inline __attribute__((always_inline)) uint32_t cflag_add(uint32_t s, uint32_t d, uint32_t res, unsigned int sz)
{
    return sz == 1 ? res : sz == 2 ? res >> 8 : ((s & d) | (~res & (s | d))) >> 23;
}
static inline __attribute__((always_inline)) uint32_t cflag_sub(uint32_t s, uint32_t d, uint32_t res, unsigned int sz)
{
    return sz == 1 ? res : sz == 2 ? res >> 8 : ((s & res) | (~d & (s | res))) >> 23;
}
static inline __attribute__((always_inline)) uint32_t vflag_add(uint32_t s, uint32_t d, uint32_t res, unsigned int sz)
{
    uint32_t v = (s ^ res) & (d ^ res);
    return sz == 1 ? v : sz == 2 ? v >> 8 : v >> 24;
}
static inline __attribute__((always_inline)) uint32_t vflag_sub(uint32_t s, uint32_t d, uint32_t res, unsigned int sz)
{
    uint32_t v = (s ^ d) & (res ^ d);
    return sz == 1 ? v : sz == 2 ? v >> 8 : v >> 24;
}

static int cond(const regs_t *r, unsigned int cc)
{
    int cs = (r->c & 0x100) != 0, ne = r->z != 0, mi = (r->n & 0x80) != 0, vs = (r->v & 0x80) != 0;
    int lt = ((r->n ^ r->v) & 0x80) != 0;
    switch (cc) {
    case 0: return 1;                 /* BRA */
    case 2: return !cs && ne;         /* HI */
    case 3: return cs || !ne;         /* LS */
    case 4: return !cs;               /* CC */
    case 5: return cs;                /* CS */
    case 6: return ne;                /* NE */
    case 7: return !ne;               /* EQ */
    case 8: return !vs;               /* VC */
    case 9: return vs;                /* VS */
    case 10: return !mi;              /* PL */
    case 11: return mi;               /* MI */
    case 12: return !lt;              /* GE */
    case 13: return lt;               /* LT */
    case 14: return !lt && ne;        /* GT */
    case 15: return lt || !ne;        /* LE */
    }
    return -1;                        /* 1 = BSR: not a condition */
}

/* DBcc's condition: 0 is T, 1 is F, the rest as Bcc's */
static int db_cond(const regs_t *r, unsigned int cc)
{
    return cc == 0 ? 1 : cc == 1 ? 0 : cond(r, cc);
}

/* one operation on the shadow. Returns 1 done, 0 not reproducible here.
 * *taken: a branch's way. */
static int exec(regs_t *r, const iop_t *o, int *taken)
{
    uint32_t s, d, res, m = mask_of(o->sz);
    unsigned int a;

    switch (o->k) {
    case K_NOP:
        return 1;
    case K_BCC:
        *taken = cond(r, o->cc);
        return *taken >= 0;
    case K_JSR:
        r->r[15] -= 4;
        return wrm(r->r[15], 4, o->ret);
    case K_RTS:
        if (!rdm(r->r[15], 4, &s) || s != o->ret)
            return 0;
        r->r[15] += 4;
        return 1;
    case K_TST:
        if (!get(r, &o->s, o->sz, &res)) return 0;
        r->n = nflag(res, o->sz); r->z = res; r->v = 0; r->c = 0;
        return 1;
    case K_CMP:
        if (!get(r, &o->s, o->sz, &s) || !get(r, &o->d, o->sz, &d)) return 0;
        res = d - s;
        r->n = nflag(res, o->sz); r->z = res & m;
        r->v = vflag_sub(s, d, res, o->sz); r->c = cflag_sub(s, d, res, o->sz);
        return 1;
    case K_ADD: case K_SUB:
        if (!get(r, &o->s, o->sz, &s)) return 0;
        if (o->d.t == T_D) d = r->r[o->d.reg] & m;
        else if (!addr_of(r, &o->d, &a) || !rdm(a, o->sz, &d)) return 0;
        if (o->k == K_ADD) {
            res = s + d;
            r->v = vflag_add(s, d, res, o->sz);
            r->c = r->x = cflag_add(s, d, res, o->sz);
        } else {
            res = d - s;
            r->v = vflag_sub(s, d, res, o->sz);
            r->c = r->x = cflag_sub(s, d, res, o->sz);
        }
        r->n = nflag(res, o->sz); r->z = res & m;
        return put(r, &o->d, o->sz, res);
    case K_AND: case K_OR: case K_EOR:
        if (!get(r, &o->s, o->sz, &s)) return 0;
        if (o->d.t == T_D) d = r->r[o->d.reg] & m;
        else if (!addr_of(r, &o->d, &a) || !rdm(a, o->sz, &d)) return 0;
        res = (o->k == K_AND ? (s & d) : o->k == K_OR ? (s | d) : (s ^ d)) & m;
        r->n = nflag(res, o->sz); r->z = res; r->v = 0; r->c = 0;
        return put(r, &o->d, o->sz, res);
    case K_MOVE:
        if (!get(r, &o->s, o->sz, &res)) return 0;
        if (!put(r, &o->d, o->sz, res)) return 0;
        r->n = nflag(res, o->sz); r->z = res; r->v = 0; r->c = 0;
        return 1;
    case K_CLR:
        if (!put(r, &o->d, o->sz, 0)) return 0;
        r->n = 0; r->v = 0; r->c = 0; r->z = 0;
        return 1;
    case K_BTST:
        if (!get(r, &o->s, 4, &s)) return 0;                 /* the bit number */
        if (o->d.t == T_D) { r->z = r->r[o->d.reg] & (1u << (s & 31)); return 1; }
        if (!get(r, &o->d, 1, &d)) return 0;
        r->z = d & (1u << (s & 7));
        return 1;
    case K_BMOD: {                                            /* BCHG / BCLR / BSET */
        uint32_t mask;
        if (!get(r, &o->s, 4, &s)) return 0;                 /* the bit number */
        if (o->d.t == T_D) {
            mask = 1u << (s & 31);
            d = r->r[o->d.reg];
            r->z = d & mask;
            r->r[o->d.reg] = o->sh == BM_CLR ? (d & ~mask) : o->sh == BM_SET ? (d | mask) : (d ^ mask);
            return 1;
        }
        mask = 1u << (s & 7);
        if (!addr_of(r, &o->d, &a) || !rdm(a, 1, &d)) return 0;
        r->z = d & mask;
        return wrm(a, 1, o->sh == BM_CLR ? (d & ~mask) : o->sh == BM_SET ? (d | mask) : (d ^ mask));
    }
    case K_DBCC: {
        if (db_cond(r, o->cc)) { *taken = DB_COND; return 1; }
        res = (r->r[o->d.reg] - 1) & 0xffff;
        r->r[o->d.reg] = (r->r[o->d.reg] & ~0xffffu) | res;
        *taken = res != 0xffff ? DB_LOOP : DB_EXPIRED;
        return 1;
    }
    case K_SHIFT: {                                           /* .W, immediate count */
        unsigned int sh = o->cnt;
        uint32_t src = r->r[o->d.reg] & 0xffff;
        switch (o->sh) {
        case SH_ROL:
            res = ((src << sh) | (src >> (16 - sh))) & 0xffff;
            r->c = src >> (8 - sh);
            break;
        case SH_ROR:
            res = ((src >> sh) | (src << (16 - sh))) & 0xffff;
            r->c = src << (9 - sh);
            break;
        case SH_LSL:
            res = (src << sh) & 0xffff;
            r->x = r->c = src >> (8 - sh);
            break;
        case SH_LSR:
            res = src >> sh;
            r->x = r->c = src << (9 - sh);
            break;
        default:
            return 0;
        }
        r->r[o->d.reg] = (r->r[o->d.reg] & ~0xffffu) | res;
        r->n = o->sh == SH_LSR ? 0 : res >> 8;
        r->z = res;
        r->v = 0;
        return 1;
    }
    }
    return 0;
}

static void load_regs(regs_t *r)
{
    memcpy(r->r, m68k.dar, sizeof r->r);
    r->x = m68k.x_flag; r->n = m68k.n_flag; r->z = m68k.not_z_flag;
    r->v = m68k.v_flag; r->c = m68k.c_flag;
}

/* one pass along the trace on the shadow (w_* restarted): 1 if every
 * operation ran and every branch went the traced way */
static int pass(regs_t *r)
{
    uint32_t sp0 = r->r[15];
    w_n = 0; w_dog = 0;
    for (int i = 0; i < nops; i++) {
        int t = 0;
        if (!exec(r, &ops[i], &t))
            return 0;
        if ((ops[i].k == K_BCC || ops[i].k == K_DBCC) && t != ops[i].taken)
            return 0;
    }
    return r->r[15] == sp0;          /* a pass leaves the stack where it found it */
}

/* ------------------------------------------------------------------ */
/* decoding                                                            */

static int ea(unsigned int mode, unsigned int reg, unsigned int sz, unsigned int *ext, opd_t *o)
{
    unsigned int w, w2;
    o->reg = (uint8_t)reg;
    switch (mode) {
    case 0: o->t = T_D; return 1;
    case 1: o->t = T_A; return 1;
    case 2: o->t = T_AIND; o->v = 0; return 1;
    case 3: if (reg != 7) return 0; o->t = T_POP; return 1;
    case 4: if (reg != 7) return 0; o->t = T_PUSH; return 1;
    case 5:
        if (!fetch16(*ext, &w)) return 0;
        *ext += 2;
        o->t = T_AIND; o->v = (uint32_t)sext16(w);
        return 1;
    case 7:
        switch (reg) {
        case 0:
            if (!fetch16(*ext, &w)) return 0;
            *ext += 2;
            o->t = T_ABS; o->v = (uint32_t)sext16(w) & 0xffffff;
            return 1;
        case 1:
            if (!fetch16(*ext, &w) || !fetch16(*ext + 2, &w2)) return 0;
            *ext += 4;
            o->t = T_ABS; o->v = ((w << 16) | w2) & 0xffffff;
            return 1;
        case 2:
            if (!fetch16(*ext, &w)) return 0;
            o->t = T_ABS; o->v = (*ext + (uint32_t)sext16(w)) & 0xffffff;
            *ext += 2;
            return 1;
        case 4:
            if (sz == 4) {
                if (!fetch16(*ext, &w) || !fetch16(*ext + 2, &w2)) return 0;
                o->v = (w << 16) | w2;
                *ext += 4;
            } else {
                if (!fetch16(*ext, &w)) return 0;
                o->v = sz == 1 ? (w & 0xff) : w;
                *ext += 2;
            }
            o->t = T_IMM;
            return 1;
        }
        return 0;
    }
    return 0;                 /* (An)+/-(An) but A7, indexed */
}

static int writable(const opd_t *o)
{
    return o->t == T_D || o->t == T_ABS || o->t == T_AIND || o->t == T_PUSH;
}

/* the instruction at pc as an operation; *len its length, *target a
 * branch's/JSR's destination. 0 if outside the subset. */
static int decode(const regs_t *r, unsigned int pc, iop_t *o, unsigned int *len, unsigned int *target)
{
    unsigned int op, ext = pc + 2, w, sz;
    static const uint8_t msz[4] = { 0, 1, 4, 2 };

    memset(o, 0, sizeof *o);
    if (!fetch16(pc, &op))
        return 0;
    o->pc = pc;
    o->cyc = m68ki_cycles[op];

    if (op == 0x4e71) { o->k = K_NOP; *len = 2; return 1; }
    if (op == 0x4e75) { o->k = K_RTS; *len = 2; return 1; }
    if ((op & 0xffc0) == 0x4e80) {                         /* JSR */
        opd_t t;
        unsigned int m = (op >> 3) & 7;
        if (m != 2 && m != 5 && m != 7) return 0;
        if (!ea(m, op & 7, 4, &ext, &t)) return 0;
        if (t.t == T_AIND) *target = r->r[8 + t.reg] + t.v;
        else if (t.t == T_ABS) *target = t.v;
        else return 0;
        o->k = K_JSR; o->ret = ext; *len = ext - pc;
        return 1;
    }
    if ((op & 0xf000) == 0x6000) {                         /* Bcc / BRA / BSR */
        unsigned int d8 = op & 0xff;
        o->cc = (uint8_t)((op >> 8) & 0xf);
        if (d8 == 0) {
            if (!fetch16(pc + 2, &w)) return 0;
            *target = pc + 2 + (unsigned int)sext16(w);
            *len = 4;
        } else {
            *target = pc + 2 + (unsigned int)(int)(int8_t)d8;
            *len = 2;
        }
        o->sz = (uint8_t)(*len == 2 ? 1 : 2);
        if (o->cc == 1) { o->k = K_JSR; o->ret = pc + *len; return 1; }   /* BSR */
        o->k = K_BCC;
        return 1;
    }
    if ((op & 0xff00) == 0x4a00 || (op & 0xff00) == 0x4200) {   /* TST / CLR */
        sz = (op >> 6) & 3;
        if (sz == 3) return 0;
        o->sz = (uint8_t)(1u << sz);
        if ((op & 0xff00) == 0x4a00) {
            o->k = K_TST;
            if (!ea((op >> 3) & 7, op & 7, o->sz, &ext, &o->s) || o->s.t == T_IMM || o->s.t == T_A)
                return 0;
        } else {
            o->k = K_CLR;
            if (!ea((op >> 3) & 7, op & 7, o->sz, &ext, &o->d) || !writable(&o->d))
                return 0;
        }
        *len = ext - pc;
        return 1;
    }
    if ((op & 0xf100) == 0x0000 && (op & 0x0e00) != 0x0800) {  /* ORI ANDI SUBI ADDI EORI CMPI */
        static const uint8_t kind[8] = { K_OR, K_AND, K_SUB, K_ADD, 0xff, K_EOR, K_CMP, 0xff };
        unsigned int grp = (op >> 9) & 7;
        sz = (op >> 6) & 3;
        if (kind[grp] == 0xff || sz == 3 || (op & 0x3f) == 0x3c) return 0;   /* CCR/SR forms */
        o->sz = (uint8_t)(1u << sz);
        o->k = kind[grp];
        if (!ea(7, 4, o->sz, &ext, &o->s)) return 0;           /* the immediate */
        if (!ea((op >> 3) & 7, op & 7, o->sz, &ext, &o->d)) return 0;
        if (o->k == K_CMP ? (o->d.t == T_IMM || o->d.t == T_A) : !writable(&o->d))
            return 0;
        if (o->k == K_CMP && o->d.t == T_PUSH) return 0;
        *len = ext - pc;
        return 1;
    }
    if (((op & 0xffc0) >= 0x0840 && (op & 0xffc0) <= 0x08c0) ||
        ((op & 0xf100) == 0x0100 && (op & 0x00c0) && (op & 0x38) != 0x08)) {   /* BCHG BCLR BSET */
        o->k = K_BMOD;
        o->sh = (uint8_t)(((op >> 6) & 3) - 1);             /* 1 CHG, 2 CLR, 3 SET */
        if ((op & 0xf100) == 0x0000) {                       /* #n */
            if (!fetch16(ext, &w)) return 0;
            ext += 2;
            o->s.t = T_IMM; o->s.v = w & 0xff;
        } else {
            o->s.t = T_D; o->s.reg = (uint8_t)((op >> 9) & 7);
        }
        if (!ea((op >> 3) & 7, op & 7, 1, &ext, &o->d) || !writable(&o->d) || o->d.t == T_PUSH)
            return 0;
        o->sz = (uint8_t)(o->d.t == T_D ? 4 : 1);
        *len = ext - pc;
        return 1;
    }
    if ((op & 0xf0f8) == 0x50c8) {                         /* DBcc Dn,disp16 */
        if (!fetch16(pc + 2, &w)) return 0;
        o->k = K_DBCC;
        o->cc = (uint8_t)((op >> 8) & 0xf);
        o->d.t = T_D; o->d.reg = (uint8_t)(op & 7);
        *target = pc + 2 + (unsigned int)sext16(w);
        *len = 4;
        return 1;
    }
    if ((op & 0xffc0) == 0x0800 || ((op & 0xf1c0) == 0x0100 && (op & 0x38) != 0x08)) {   /* BTST */
        o->k = K_BTST;
        if ((op & 0xffc0) == 0x0800) {
            if (!fetch16(ext, &w)) return 0;
            ext += 2;
            o->s.t = T_IMM; o->s.v = w & 0xff;
        } else {
            o->s.t = T_D; o->s.reg = (uint8_t)((op >> 9) & 7);
        }
        if (!ea((op >> 3) & 7, op & 7, 1, &ext, &o->d) || o->d.t == T_A || o->d.t == T_PUSH)
            return 0;
        if (o->d.t == T_IMM && (op & 0xffc0) == 0x0800) return 0;
        *len = ext - pc;
        return 1;
    }
    if ((op & 0xc000) == 0 && (op & 0x3000)) {             /* MOVE (not MOVEA) */
        o->sz = msz[(op >> 12) & 3];
        o->k = K_MOVE;
        if (((op >> 6) & 7) == 1) return 0;
        if (!ea((op >> 3) & 7, op & 7, o->sz, &ext, &o->s)) return 0;
        if (!ea((op >> 6) & 7, (op >> 9) & 7, o->sz, &ext, &o->d) || !writable(&o->d))
            return 0;
        *len = ext - pc;
        return 1;
    }
    if ((op & 0xf000) == 0x5000 && ((op >> 6) & 3) != 3) {  /* ADDQ / SUBQ */
        o->sz = (uint8_t)(1u << ((op >> 6) & 3));
        o->k = (op & 0x0100) ? K_SUB : K_ADD;
        o->s.t = T_IMM; o->s.v = (((op >> 9) - 1) & 7) + 1;
        if (!ea((op >> 3) & 7, op & 7, o->sz, &ext, &o->d) || !writable(&o->d) || o->d.t == T_PUSH)
            return 0;
        *len = ext - pc;
        return 1;
    }
    {   /* OR SUB CMP/EOR AND ADD with a data register */
        unsigned int hi = op & 0xf000, om = (op >> 6) & 7, dn = (op >> 9) & 7, m = (op >> 3) & 7;
        int k = hi == 0x8000 ? K_OR : hi == 0x9000 ? K_SUB : hi == 0xb000 ? K_CMP :
                hi == 0xc000 ? K_AND : hi == 0xd000 ? K_ADD : -1;
        if (k >= 0 && om != 3 && om != 7) {
            o->sz = (uint8_t)(1u << (om & 3));
            if (om < 3) {                                   /* <ea> op Dn -> Dn */
                o->k = (uint8_t)k;
                if (!ea(m, op & 7, o->sz, &ext, &o->s)) return 0;
                if (o->s.t == T_PUSH) return 0;
                o->d.t = T_D; o->d.reg = (uint8_t)dn;
            } else {                                        /* Dn op <ea> -> <ea> */
                if (k == K_CMP) k = K_EOR;
                o->k = (uint8_t)k;
                if (m <= 1) return 0;                       /* SBCD/SUBX/CMPM/ABCD/EXG/ADDX */
                o->s.t = T_D; o->s.reg = (uint8_t)dn;
                if (!ea(m, op & 7, o->sz, &ext, &o->d) || !writable(&o->d) || o->d.t == T_PUSH)
                    return 0;
            }
            if (k == K_EOR && om < 3) return 0;
            *len = ext - pc;
            return 1;
        }
    }
    if ((op & 0xf000) == 0xe000 && ((op >> 6) & 3) == 1 && !(op & 0x20)) {   /* shift .W #n,Dn */
        unsigned int ty = (op >> 3) & 3, left = (op >> 8) & 1;
        if (ty == 2) return 0;                                /* ROXL/ROXR */
        o->k = K_SHIFT; o->sz = 2;
        o->sh = (uint8_t)(ty == 0 ? (left ? SH_ASL : SH_ASR) : ty == 1 ? (left ? SH_LSL : SH_LSR)
                                  : (left ? SH_ROL : SH_ROR));
        if (o->sh == SH_ASL || o->sh == SH_ASR) return 0;
        o->cnt = (uint8_t)((((op >> 9) - 1) & 7) + 1);
        o->d.t = T_D; o->d.reg = (uint8_t)(op & 7);
        o->cyc = (uint16_t)(o->cyc + o->cnt * 2);              /* USE_CYCLES(shift * CYC_SHIFT) */
        *len = 2;
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* trace, arm, skip                                                    */

/* one pass from the head on the shadow, recorded (header comment, 2) */
static int trace(unsigned int head)
{
    regs_t r;
    unsigned int pc = head, len, target, cyc = 0;

    load_regs(&r);
    trace_sp = r.r[15];
    w_n = 0; w_dog = 0;
    for (nops = 0; nops < MAXOPS; nops++) {
        iop_t *o = &ops[nops];
        int t = 0;
        target = 0;
        if (!decode(&r, pc, o, &len, &target))
            return 0;
        if (o->k == K_RTS && !rdm(r.r[15], 4, &o->ret))     /* the pop this pass makes */
            return 0;
        if (!exec(&r, o, &t))
            return 0;
        if (o->k == K_BCC) {
            o->taken = (uint8_t)t;
            if (!t)
                o->cyc = (uint16_t)(o->cyc + (len == 2 ? -2 : 2));   /* CYC_BCC_NOTAKE_B/W */
            pc = t ? target : pc + len;
        } else if (o->k == K_DBCC) {
            o->taken = (uint8_t)t;
            if (t == DB_LOOP) o->cyc = (uint16_t)(o->cyc - 2);       /* CYC_DBCC_F_NOEXP */
            if (t == DB_EXPIRED) o->cyc = (uint16_t)(o->cyc + 2);    /* CYC_DBCC_F_EXP */
            pc = t == DB_LOOP ? target : pc + len;
        } else if (o->k == K_JSR) {
            pc = target;
        } else if (o->k == K_RTS) {
            pc = o->ret;
        } else {
            pc += len;
        }
        cyc += o->cyc;
        if ((pc & 0xffffff) == head && r.r[15] == trace_sp) {
            nops++;
            neo_idle_lap = cyc;
            return 1;
        }
    }
    return 0;
}

static void liveness(void);

/* RTS ops carry the value they must pop: fill them from the JSRs */
static int link_returns(void)
{
    uint32_t stack[8];
    int sp = 0;
    for (int i = 0; i < nops; i++) {
        if (ops[i].k == K_JSR) {
            if (sp == 8) return 0;
            stack[sp++] = ops[i].ret;
        } else if (ops[i].k == K_RTS) {
            if (!sp) return 0;                 /* returns out of the loop */
            ops[i].ret = stack[--sp];
        }
    }
    return sp == 0;
}

void neo_idle_reset(void)
{
    neo_idle_head = NEO_IDLE_NONE;
    neo_idle_lap = neo_idle_last = 0;
    nops = 0;
    rej_n = 0;
    memset(rej, 0xff, sizeof rej);
}

static void host_env(void)
{
#ifdef HOST_BUILD
    if (host_off < 0) {
        const char *e = getenv("NEO_IDLE_LOG");
        host_off = getenv("NEO_NOIDLE") != NULL;
        neo_idle_log = e ? (unsigned)atoi(e) : 0;
        if (e && !neo_idle_log) neo_idle_log = 1;
        neo_idle_bug = getenv("NEO_IDLEBUG") != NULL;
    }
#else
    host_off = 0;
#endif
}

static void reject_pc(unsigned int pc, const char *why)
{
    rej[rej_n++ & 7] = pc;
    neo_idle_rejects++;
#ifdef HOST_BUILD
    if (neo_idle_log)
        printf("IDLE: no skip at %06x (%s)\n", pc, why);
#else
    (void)why;
#endif
}

/* the first time the head comes round after arming: trace a pass */
void neo_idle_trace(void)
{
    unsigned int head = neo_idle_head;
    if (trace(head) && link_returns()) {
        /* exec checked the pops against the shadow; re-run one pass with
         * the linked expectations to be sure the trace replays */
        regs_t r;
        load_regs(&r);
        if (pass(&r)) {
            trace_fails = 0;
            liveness();
#ifdef HOST_BUILD
            if (neo_idle_log) {
                printf("IDLE: traced %06x: %d operations, %u cycles a pass:", head, nops, neo_idle_lap);
                for (int i = 0; i < nops; i++) printf(" %06x", ops[i].pc);
                printf("\n");
            }
#endif
            return;
        }
    }
    neo_idle_lap = 0;
    nops = 0;
    if (++trace_fails >= 4) {
        reject_pc(head, "no pass traced from the head");
        neo_idle_head = NEO_IDLE_NONE;
        trace_fails = 0;
    }
}

/* ------------------------------------------------------------------ */
/* the in-place runner (RAM, hot): the traced operations with their memory
 * operands bound to host pointers once per skip -- A0-A6 do not change
 * during a run; A7-relative ones go through the stack's 64 KiB block,
 * bound once too (the stack stays in work RAM, away from the block's
 * edges) -- run on the real registers, flags and memory.
 *
 * FLAGS: a pass computes only the flags a later branch reads before they
 * are overwritten (liveness(), round the loop); the last pass computes
 * them all, so Musashi carries on from exactly its own flags. A pass that
 * leaves the traced path (a branch the other way, an RTS popping another
 * address) is rolled back -- registers, flags and an undo log of its
 * stores, from the checkpoint taken at its start -- and replayed with
 * every flag computed, to stop BEFORE the instruction where it leaves:
 * every instruction boundary is a state Musashi could be in, so the PC is
 * set there, with the exact flags, and Musashi carries on. */

enum { B_D, B_A, B_IMM, B_MEM, B_STK, B_PUSH, B_POP, B_WDOG };
typedef struct {
    uint8_t k;               /* B_* */
    uint8_t reg;
    uint32_t v;              /* immediate, or the A7 displacement */
    uint8_t *p8;             /* B_MEM: the byte, or the high word / low word */
    uint16_t *p16, *q16;
} bop_t;

typedef struct { uint8_t *p; uint16_t old; uint8_t word; } undo_t;
#define MAXU MAXW

static bop_t *bs, *bd;               /* the operations' bound operands (idle_mem) */
static uint16_t *cyc_before;
static undo_t *un;
static uint8_t *stack_base;          /* the stack's 64 KiB block, bound per skip */

/* the working memory, from the AHB heap at the first arm (not the core's
 * RAM pool, which a big game's 68000 jump table leaves a few KiB of) */
static int idle_mem(void)
{
    static uint8_t *blk;
    size_t n = MAXOPS * (sizeof(iop_t) + 2 * sizeof(bop_t)) + MAXW * (sizeof(uint32_t) + 1) +
               MAXU * sizeof(undo_t) + (MAXOPS + 1) * sizeof(uint16_t) + 32;
    if (blk)
        return 1;
    blk = (uint8_t *)ahb_calloc(1, n);
    if (!blk)
        blk = (uint8_t *)ram_calloc(1, n);
    if (!blk)
        return 0;
    uint8_t *p = blk;
    ops = (iop_t *)p;           p += MAXOPS * sizeof(iop_t);
    bs = (bop_t *)p;            p += MAXOPS * sizeof(bop_t);
    bd = (bop_t *)p;            p += MAXOPS * sizeof(bop_t);
    un = (undo_t *)p;           p += MAXU * sizeof(undo_t);
    w_a = (uint32_t *)p;        p += MAXW * sizeof(uint32_t);
    cyc_before = (uint16_t *)p; p += (MAXOPS + 1) * sizeof(uint16_t);
    w_v = p;
    return 1;
}

static uint8_t *host_byte(unsigned int a) { return map_of(a)->base + ((a ^ 1) & 0xffff); }
static uint16_t *host_word(unsigned int a) { return (uint16_t *)(map_of(a)->base + (a & 0xffff)); }

/* bind one operand; write: it is stored to. 0 if not bindable */
static int bind(const opd_t *o, unsigned int sz, int write, bop_t *b)
{
    unsigned int a;
    memset(b, 0, sizeof *b);
    b->reg = o->reg;
    switch (o->t) {
    case T_NONE: b->k = B_IMM; return 1;
    case T_D: b->k = B_D; return 1;
    case T_A: b->k = B_A; return !write;
    case T_IMM: b->k = B_IMM; b->v = o->v; return !write;
    case T_PUSH: b->k = B_PUSH; return 1;
    case T_POP: b->k = B_POP; return !write;
    case T_ABS: a = o->v; break;
    case T_AIND:
        if (o->reg == 7) { b->k = B_STK; b->v = o->v; return 1; }
        a = (m68k.dar[8 + o->reg] + o->v) & 0xffffff;
        break;
    default: return 0;
    }
    a &= 0xffffff;
    if (sz > 1 && (a & 1)) return 0;
    if (write && (a & ~1u) == 0x300000) { b->k = B_WDOG; return 1; }
    for (unsigned int i = 0; i < sz; i++)
        if (!(write ? ram_write(a + i) : direct_read(a + i))) return 0;
    if (write)                       /* a read-modify-write reads it too */
        for (unsigned int i = 0; i < sz; i++)
            if (!direct_read(a + i)) return 0;
    b->k = B_MEM;
    b->p8 = host_byte(a);
    b->p16 = host_word(a);
    b->q16 = host_word(a + 2);
    return 1;
}

/* the stack stays in work RAM, away from its 64 KiB block's edges, for a
 * run: then base + (address & 0xffff) is every A7 access */
static int bind_stack(void)
{
    unsigned int sp = m68k.dar[15] & 0xffffff;
    if (!ram_write(sp) || (sp & 0xffff) < 0x100 || (sp & 0xffff) > 0xff00 ||
        map_of(sp)->base != map_of(sp - 0x100)->base || map_of(sp)->base != map_of(sp + 0xff)->base)
        return 0;
    stack_base = map_of(sp)->base;
    return 1;
}

/* the flags: N Z V C X */
#define F_N 1
#define F_Z 2
#define F_V 4
#define F_C 8
#define F_X 16
#define F_ALL 31

static unsigned int flags_written(const iop_t *o)
{
    switch (o->k) {
    case K_TST: case K_CMP: case K_AND: case K_OR: case K_EOR: case K_MOVE: case K_CLR:
        return F_N | F_Z | F_V | F_C;
    case K_ADD: case K_SUB:
        return F_ALL;
    case K_BTST: case K_BMOD:
        return F_Z;
    case K_SHIFT:
        return (o->sh == SH_LSL || o->sh == SH_LSR) ? F_ALL : (F_N | F_Z | F_V | F_C);
    }
    return 0;
}

static unsigned int flags_read(const iop_t *o)
{
    if (o->k != K_BCC && o->k != K_DBCC)
        return 0;
    switch (o->cc) {
    case 0: case 1: return 0;
    case 2: case 3: return F_C | F_Z;
    case 4: case 5: return F_C;
    case 6: case 7: return F_Z;
    case 8: case 9: return F_V;
    case 10: case 11: return F_N;
    case 12: case 13: return F_N | F_V;
    default: return F_N | F_V | F_Z;
    }
}

/* which flags each operation must produce: those a branch reads before an
 * operation overwrites them, round the loop (the pass repeats) */
static void liveness(void)
{
    unsigned int live = 0;
    for (int round = 0; round < 2; round++)
        for (int i = nops - 1; i >= 0; i--) {
            unsigned int w = flags_written(&ops[i]);
            ops[i].need = (uint8_t)(w & live);
            live = (live & ~w) | flags_read(&ops[i]);
        }
}

#define NEO_RUNNER __attribute__((section(".ram_idle"), noinline))
#define AI static inline __attribute__((always_inline))
#define RH static __attribute__((section(".ram_idle"), noinline))    /* runner helpers: RAM, called */

RH uint32_t stk_rd(const uint8_t *sb, unsigned int a, unsigned int sz)
{
    if (sz == 1) return sb[(a ^ 1) & 0xffff];
    if (sz == 2) return *(const uint16_t *)(sb + (a & 0xffff));
    return ((uint32_t)*(const uint16_t *)(sb + (a & 0xffff)) << 16) |
           *(const uint16_t *)(sb + ((a + 2) & 0xffff));
}

/* a store; with track, the bytes it overwrites go to the undo log */
AI void put16(uint16_t *p, uint16_t v, int track, int *nu)
{
    if (track) { un[*nu].p = (uint8_t *)p; un[*nu].old = *p; un[*nu].word = 1; (*nu)++; }
    *p = v;
}
AI void put8(uint8_t *p, uint8_t v, int track, int *nu)
{
    if (track) { un[*nu].p = p; un[*nu].old = *p; un[*nu].word = 0; (*nu)++; }
    *p = v;
}

RH void stk_wr(uint8_t *sb, unsigned int a, unsigned int sz, uint32_t v, int track, int *nu)
{
    if (sz == 1) { put8(sb + ((a ^ 1) & 0xffff), (uint8_t)v, track, nu); return; }
    if (sz == 2) { put16((uint16_t *)(sb + (a & 0xffff)), (uint16_t)v, track, nu); return; }
    put16((uint16_t *)(sb + (a & 0xffff)), (uint16_t)(v >> 16), track, nu);
    put16((uint16_t *)(sb + ((a + 2) & 0xffff)), (uint16_t)v, track, nu);
}

RH uint32_t mem_rd(const bop_t *b, unsigned int sz)
{
    if (sz == 1) return *b->p8;
    if (sz == 2) return *b->p16;
    return ((uint32_t)*b->p16 << 16) | *b->q16;
}

RH void mem_wr(const bop_t *b, unsigned int sz, uint32_t v, int track, int *nu)
{
    if (sz == 1) { put8(b->p8, (uint8_t)v, track, nu); return; }
    if (sz == 2) { put16(b->p16, (uint16_t)v, track, nu); return; }
    put16(b->p16, (uint16_t)(v >> 16), track, nu);
    put16(b->q16, (uint16_t)v, track, nu);
}

RH uint32_t fget(unsigned int *R, const uint8_t *sb, const bop_t *b, unsigned int sz)
{
    unsigned int step = sz == 1 ? 2 : sz;
    uint32_t v;
    switch (b->k) {
    case B_D: return R[b->reg] & mask_of(sz);
    case B_A: return R[8 + b->reg] & mask_of(sz);
    case B_IMM: return b->v & mask_of(sz);
    case B_MEM: return mem_rd(b, sz);
    case B_STK: return stk_rd(sb, R[15] + b->v, sz);
    case B_POP: v = stk_rd(sb, R[15], sz); R[15] += step; return v;
    }
    return 0;
}

RH void fput(unsigned int *R, uint8_t *sb, const bop_t *b, unsigned int sz, uint32_t v, int track, int *nu,
             int *wdog)
{
    uint32_t m = mask_of(sz);
    unsigned int step = sz == 1 ? 2 : sz;
    switch (b->k) {
    case B_D: R[b->reg] = (R[b->reg] & ~m) | (v & m); return;
    case B_MEM: mem_wr(b, sz, v, track, nu); return;
    case B_STK: stk_wr(sb, R[15] + b->v, sz, v, track, nu); return;
    case B_PUSH: R[15] -= step; stk_wr(sb, R[15], sz, v, track, nu); return;
    case B_WDOG: *wdog = 1; return;
    }
}

/* up to `passes` passes; returns the cycles used (m68k.pc moved if a pass
 * stopped half way) */
#ifdef HOST_BUILD
/* NEO_IDLE_STOPTEST=1 (host): every run pretends its middle pass leaves
 * the path at its last branch -- the rollback, the replay with every flag
 * and Musashi's take-over half way through a pass, which real loops reach
 * only when they exit during a run; the hashes must not change */
static int stoptest = -1;
#endif

NEO_RUNNER static unsigned int run_in_place(unsigned int passes)
{
    /* the pass's records and the undo log on the stack (DTCM, zero-wait):
     * copied once per run, the loop below reads nothing else from the
     * heap */
    iop_t lops[MAXOPS];
    bop_t lbs[MAXOPS], lbd[MAXOPS];
    undo_t lun[MAXU];
    uint16_t lcyc[MAXOPS + 1];
    unsigned int *R = m68k.dar;
    uint8_t *sb = stack_base;
    uint32_t x = m68k.x_flag, n = m68k.n_flag, z = m68k.not_z_flag, v = m68k.v_flag, c = m68k.c_flag;
    uint32_t ck_r[16], ck_x = 0, ck_n = 0, ck_z = 0, ck_v = 0, ck_c = 0;
    unsigned int done = 0, i = 0, nf = (unsigned int)nops;
    int full, track, nu, wdog;
    uint32_t s, d, res, m;

    memcpy(lops, ops, nf * sizeof(iop_t));
    memcpy(lbs, bs, nf * sizeof(bop_t));
    memcpy(lbd, bd, nf * sizeof(bop_t));
    memcpy(lcyc, cyc_before, (nf + 1) * sizeof(uint16_t));
    un = lun;                                /* put8/put16's log, for this run */
#ifdef HOST_BUILD
    unsigned int lastbcc = 0;
    if (stoptest < 0) stoptest = getenv("NEO_IDLE_STOPTEST") != NULL;
    for (unsigned int k = 0; k < nf; k++) if (ops[k].k == K_BCC) lastbcc = k;
#endif

    while (done < passes) {
        full = done == passes - 1;           /* the last pass computes every flag */
pass_start:
        track = !full;
        if (track) {                          /* the checkpoint */
            for (int k = 0; k < 16; k++) ck_r[k] = R[k];
            ck_x = x; ck_n = n; ck_z = z; ck_v = v; ck_c = c;
        }
        nu = 0;
        wdog = 0;
        uint32_t sp0 = R[15];
        for (i = 0; i < nf; i++) {
            const iop_t *o = &lops[i];
            const bop_t *S = &lbs[i], *D = &lbd[i];
            unsigned int sz = o->sz, nd = full ? F_ALL : o->need;
            m = mask_of(sz);
            switch (o->k) {
            case K_NOP:
                break;
            case K_BCC: {
                int t;
                switch (o->cc) {
                case 0: t = 1; break;
                case 2: t = !(c & 0x100) && z; break;
                case 3: t = (c & 0x100) || !z; break;
                case 4: t = !(c & 0x100); break;
                case 5: t = (c & 0x100) != 0; break;
                case 6: t = z != 0; break;
                case 7: t = !z; break;
                case 8: t = !(v & 0x80); break;
                case 9: t = (v & 0x80) != 0; break;
                case 10: t = !(n & 0x80); break;
                case 11: t = (n & 0x80) != 0; break;
                case 12: t = !((n ^ v) & 0x80); break;
                case 13: t = ((n ^ v) & 0x80) != 0; break;
                case 14: t = !((n ^ v) & 0x80) && z; break;
                default: t = ((n ^ v) & 0x80) || !z; break;
                }
#ifdef HOST_BUILD
                if (stoptest && done == passes / 2 && i == lastbcc)
                    t = !o->taken;
#endif
                if (t != o->taken)
                    goto off_path;
                break;
            }
            case K_JSR:
                R[15] -= 4;
                stk_wr(sb, R[15], 4, o->ret, track, &nu);
                break;
            case K_RTS:
                if (stk_rd(sb, R[15], 4) != o->ret)
                    goto off_path;
                R[15] += 4;
                break;
            case K_TST:
                res = fget(R, sb, S, sz);
                if (nd & F_N) n = nflag(res, sz);
                if (nd & F_Z) z = res;
                if (nd & F_V) v = 0;
                if (nd & F_C) c = 0;
                break;
            case K_CMP:
                s = fget(R, sb, S, sz); d = fget(R, sb, D, sz);
                res = d - s;
                if (nd & F_N) n = nflag(res, sz);
                if (nd & F_Z) z = res & m;
                if (nd & F_V) v = vflag_sub(s, d, res, sz);
                if (nd & F_C) c = cflag_sub(s, d, res, sz);
                break;
            case K_ADD: case K_SUB:
                s = fget(R, sb, S, sz);
                d = D->k == B_D ? (R[D->reg] & m) : D->k == B_MEM ? mem_rd(D, sz) : stk_rd(sb, R[15] + D->v, sz);
                if (o->k == K_ADD) {
                    res = s + d;
                    if (nd & F_V) v = vflag_add(s, d, res, sz);
                    if (nd & (F_C | F_X)) { uint32_t cf = cflag_add(s, d, res, sz); if (nd & F_C) c = cf; if (nd & F_X) x = cf; }
                } else {
                    res = d - s;
                    if (nd & F_V) v = vflag_sub(s, d, res, sz);
                    if (nd & (F_C | F_X)) { uint32_t cf = cflag_sub(s, d, res, sz); if (nd & F_C) c = cf; if (nd & F_X) x = cf; }
                }
                if (nd & F_N) n = nflag(res, sz);
                if (nd & F_Z) z = res & m;
                fput(R, sb, D, sz, res, track, &nu, &wdog);
                break;
            case K_AND: case K_OR: case K_EOR:
                s = fget(R, sb, S, sz);
                d = D->k == B_D ? (R[D->reg] & m) : D->k == B_MEM ? mem_rd(D, sz) : stk_rd(sb, R[15] + D->v, sz);
                res = (o->k == K_AND ? (s & d) : o->k == K_OR ? (s | d) : (s ^ d)) & m;
                if (nd & F_N) n = nflag(res, sz);
                if (nd & F_Z) z = res;
                if (nd & F_V) v = 0;
                if (nd & F_C) c = 0;
                fput(R, sb, D, sz, res, track, &nu, &wdog);
                break;
            case K_MOVE:
                res = fget(R, sb, S, sz);
                fput(R, sb, D, sz, res, track, &nu, &wdog);
                if (nd & F_N) n = nflag(res, sz);
                if (nd & F_Z) z = res;
                if (nd & F_V) v = 0;
                if (nd & F_C) c = 0;
                break;
            case K_CLR:
                fput(R, sb, D, sz, 0, track, &nu, &wdog);
                if (nd & F_N) n = 0;
                if (nd & F_Z) z = 0;
                if (nd & F_V) v = 0;
                if (nd & F_C) c = 0;
                break;
            case K_BTST:
                s = fget(R, sb, S, 4);
                if (nd & F_Z) {
                    if (D->k == B_D) z = R[D->reg] & (1u << (s & 31));
                    else z = fget(R, sb, D, 1) & (1u << (s & 7));
                }
                break;
            case K_BMOD: {
                uint32_t mask, nv;
                s = fget(R, sb, S, 4);
                if (D->k == B_D) {
                    mask = 1u << (s & 31);
                    d = R[D->reg];
                    if (nd & F_Z) z = d & mask;
                    R[D->reg] = o->sh == BM_CLR ? (d & ~mask) : o->sh == BM_SET ? (d | mask) : (d ^ mask);
                } else {
                    mask = 1u << (s & 7);
                    d = D->k == B_MEM ? *D->p8 : stk_rd(sb, R[15] + D->v, 1);
                    if (nd & F_Z) z = d & mask;
                    nv = o->sh == BM_CLR ? (d & ~mask) : o->sh == BM_SET ? (d | mask) : (d ^ mask);
                    fput(R, sb, D, 1, nv, track, &nu, &wdog);
                }
                break;
            }
            case K_DBCC: {
                int cnd, way;
                switch (o->cc) {
                case 0: cnd = 1; break;
                case 1: cnd = 0; break;
                case 2: cnd = !(c & 0x100) && z; break;
                case 3: cnd = (c & 0x100) || !z; break;
                case 4: cnd = !(c & 0x100); break;
                case 5: cnd = (c & 0x100) != 0; break;
                case 6: cnd = z != 0; break;
                case 7: cnd = !z; break;
                case 8: cnd = !(v & 0x80); break;
                case 9: cnd = (v & 0x80) != 0; break;
                case 10: cnd = !(n & 0x80); break;
                case 11: cnd = (n & 0x80) != 0; break;
                case 12: cnd = !((n ^ v) & 0x80); break;
                case 13: cnd = ((n ^ v) & 0x80) != 0; break;
                case 14: cnd = !((n ^ v) & 0x80) && z; break;
                default: cnd = ((n ^ v) & 0x80) || !z; break;
                }
                res = (R[o->d.reg] - 1) & 0xffff;
                way = cnd ? DB_COND : res != 0xffff ? DB_LOOP : DB_EXPIRED;
                if (way != o->taken)
                    goto off_path;            /* before the decrement: nothing done */
                if (!cnd)
                    R[o->d.reg] = (R[o->d.reg] & ~0xffffu) | res;
                break;
            }
            case K_SHIFT: {
                unsigned int sh = o->cnt;
                uint32_t src = R[o->d.reg] & 0xffff, cf;
                switch (o->sh) {
                case SH_ROL: res = ((src << sh) | (src >> (16 - sh))) & 0xffff; cf = src >> (8 - sh); break;
                case SH_ROR: res = ((src >> sh) | (src << (16 - sh))) & 0xffff; cf = src << (9 - sh); break;
                case SH_LSL: res = (src << sh) & 0xffff; cf = src >> (8 - sh); break;
                default:     res = src >> sh; cf = src << (9 - sh); break;
                }
                R[o->d.reg] = (R[o->d.reg] & ~0xffffu) | res;
                if (nd & F_C) c = cf;
                if ((nd & F_X) && (o->sh == SH_LSL || o->sh == SH_LSR)) x = cf;
                if (nd & F_N) n = o->sh == SH_LSR ? 0 : res >> 8;
                if (nd & F_Z) z = res;
                if (nd & F_V) v = 0;
                break;
            }
            }
        }
        if (wdog)
            memory.watchdog = 0;
        done++;
        if (R[15] != sp0)                    /* a pass that moves the stack: no more */
            break;
    }
    m68k.x_flag = x; m68k.n_flag = n; m68k.not_z_flag = z; m68k.v_flag = v; m68k.c_flag = c;
    return done * neo_idle_lap;

off_path:
    if (!full) {
        /* roll the pass back and replay it with every flag computed */
        while (nu > 0) {
            nu--;
            if (un[nu].word) *(uint16_t *)un[nu].p = un[nu].old;
            else *un[nu].p = (uint8_t)un[nu].old;
        }
        for (int k = 0; k < 16; k++) R[k] = ck_r[k];
        x = ck_x; n = ck_n; z = ck_z; v = ck_v; c = ck_c;
        full = 1;
        goto pass_start;
    }
    /* stop before ops[i] (its watchdog kick, if any, not reached) */
    if (wdog)
        memory.watchdog = 0;
    m68k.pc = lops[i].pc;
    m68k.x_flag = x; m68k.n_flag = n; m68k.not_z_flag = z; m68k.v_flag = v; m68k.c_flag = c;
    return done * neo_idle_lap + lcyc[i];
}

#if defined(NEO_PROF) && !defined(HOST_BUILD)
#define IDLE_NOW() (*(volatile uint32_t *)0xE0001004u)
unsigned int neo_idle_t_setup, neo_idle_t_run, neo_idle_t_arm;
#else
#define IDLE_NOW() 0u
#endif

#ifdef HOST_BUILD
/* NEO_IDLESTAT=1 (host): per wait loop, the passes jumped over at once and
 * the passes run in place, in 68000 cycles, printed at exit -- the census
 * of which loops the runner pays for (tools/neo_idle_census.sh). */
static struct { unsigned int head, lap; unsigned long long jumped, ran; } idlestat[32];
static int idlestat_on = -1;
static unsigned int idlestat_frame_ran, idlestat_frame_jumped;
static void idlestat_dump(void)
{
    for (int i = 0; i < 32 && idlestat[i].lap; i++)
        printf("IDLESTAT: head %06x lap %u cycles: jumped %llu cycles, run in place %llu cycles\n",
               idlestat[i].head, idlestat[i].lap, idlestat[i].jumped, idlestat[i].ran);
}
static void idlestat_add(unsigned int cyc, int ran)
{
    int i;
    if (idlestat_on < 0) {
        idlestat_on = getenv("NEO_IDLESTAT") != NULL;
        if (idlestat_on) atexit(idlestat_dump);
    }
    if (!idlestat_on)
        return;
    for (i = 0; i < 31 && idlestat[i].lap && idlestat[i].head != neo_idle_head; i++)
        ;
    idlestat[i].head = neo_idle_head;
    idlestat[i].lap = neo_idle_lap;
    if (ran) idlestat[i].ran += cyc; else idlestat[i].jumped += cyc;
    if (neo_idle_head < 0xc00000) {
        if (ran) idlestat_frame_ran += cyc; else idlestat_frame_jumped += cyc;
    }
}
/* at the end of every frame: the game's own loops (not the BIOS's) in that
 * frame, "IDLEFRAME <run in place> <jumped>" in 68000 cycles, for the
 * share left in the busiest frames (tools/neo_idle_frames.py) */
void neo_idlestat_frame(void)
{
    if (idlestat_on < 0) {
        idlestat_on = getenv("NEO_IDLESTAT") != NULL;
        if (idlestat_on) atexit(idlestat_dump);
    }
    if (idlestat_on)
        printf("IDLEFRAME %u %u\n", idlestat_frame_ran, idlestat_frame_jumped);
    idlestat_frame_ran = idlestat_frame_jumped = 0;
}
#else
#define idlestat_add(c, r) ((void)0)
#endif

/* SKIP: from the head, with `laps` whole passes allowed (header comment,
 * 3); returns the cycles taken. A pass on the shadow first: if it leaves
 * everything as it is, every pass does, and all are consumed at once. */
unsigned int neo_idle_run(unsigned int laps)
{
    regs_t r, r0;
    uint32_t t0 = IDLE_NOW();
    load_regs(&r);
    r0 = r;
    if (!pass(&r))
        return 0;
    int same = memcmp(r.r, r0.r, sizeof r.r) == 0 && r.x == r0.x && r.n == r0.n &&
               r.z == r0.z && r.v == r0.v && r.c == r0.c && (!w_dog || memory.watchdog == 0);
    for (int i = 0; same && i < w_n; i++)
        if (map_of(w_a[i])->base[(w_a[i] ^ 1) & 0xffff] != w_v[i])
            same = 0;
    if (same) {
#if defined(NEO_PROF) && !defined(HOST_BUILD)
        neo_idle_t_setup += IDLE_NOW() - t0;
#endif
        idlestat_add(laps * neo_idle_lap, 0);
        return laps * neo_idle_lap;
    }

    /* the passes change the machine: run them in place */
    if (!bind_stack())
        return 0;
    unsigned int c = 0;
    for (int i = 0; i < nops; i++) {
        cyc_before[i] = (uint16_t)c;
        c += ops[i].cyc;
        int w = ops[i].k == K_ADD || ops[i].k == K_SUB || ops[i].k == K_AND ||
                ops[i].k == K_OR || ops[i].k == K_EOR || ops[i].k == K_MOVE || ops[i].k == K_CLR ||
                ops[i].k == K_BMOD;
        int bit = ops[i].k == K_BTST || ops[i].k == K_BMOD;
        if (!bind(&ops[i].s, bit ? 4 : ops[i].sz, 0, &bs[i]) ||
            !bind(&ops[i].d, ops[i].k == K_BTST ? 1 : ops[i].sz, w, &bd[i]))
            return 0;
    }
    cyc_before[nops] = (uint16_t)c;
    uint32_t t1 = IDLE_NOW();
    unsigned int used = run_in_place(laps);
#if defined(NEO_PROF) && !defined(HOST_BUILD)
    neo_idle_t_setup += t1 - t0;
    neo_idle_t_run += IDLE_NOW() - t1;
#else
    (void)t0; (void)t1;
#endif
    neo_idle_ran += used / (neo_idle_lap ? neo_idle_lap : 1);
    idlestat_add(used, 1);
    return used;
}

/* ARM: the loop the slice ended in (header comment, 1) */
static void slice_end(void);

/* ARM, timed in PROF builds */
void neo_idle_slice_end(void)
{
#if defined(NEO_PROF) && !defined(HOST_BUILD)
    uint32_t t0 = IDLE_NOW();
    slice_end();
    neo_idle_t_arm += IDLE_NOW() - t0;
#else
    slice_end();
#endif
}

static void slice_end(void)
{
    unsigned int pc = m68k.pc & 0xffffff, p = pc, len, target;
    regs_t r;
    iop_t o;

    host_env();
    if (host_off)
        return;
#ifdef HOST_BUILD
    if (neo_idle_log > 2)
        printf("IDLE: slice end at %06x\n", pc);
#endif
    if (neo_idle_head != NEO_IDLE_NONE && pc >= neo_idle_head && pc < neo_idle_head + NEO_IDLE_MAXB)
        return;
    for (unsigned int i = 0; i < 8; i++)
        if (rej[i] == pc)
            return;
    if (!idle_mem()) {
        reject_pc(pc, "no memory for the skip");
        return;
    }
    load_regs(&r);
    for (int i = 0; i < 16; i++) {
        target = 0;
        if (!decode(&r, p, &o, &len, &target)) {
            reject_pc(pc, "an instruction outside the subset");
            return;
        }
        if ((o.k == K_BCC || o.k == K_DBCC) && target <= pc && pc - target <= NEO_IDLE_MAXB) {
            neo_idle_head = target & 0xffffff;
            neo_idle_lap = neo_idle_last = 0;
            nops = 0;
            trace_fails = 0;
            neo_idle_arms++;
#ifdef HOST_BUILD
            if (neo_idle_log)
                printf("IDLE: armed head %06x (slice end at %06x)\n", neo_idle_head, pc);
#endif
            return;
        }
        if ((o.k == K_BCC && o.cc == 0) || o.k == K_JSR || o.k == K_RTS) {
            reject_pc(pc, "no backward branch before a jump");
            return;
        }
        p += len;
    }
    reject_pc(pc, "no backward branch in 16 instructions");
}

#endif
