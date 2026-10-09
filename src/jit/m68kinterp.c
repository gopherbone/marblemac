/* A compact 68000 interpreter: the JIT's cold tier.  See m68kjit_int.h.
 *
 * On the Playdate, code and data both live in PSRAM behind 16KB caches,
 * and a miss costs ~1.5us.  Translated code is ~2.5x bigger than the 68k
 * code it came from, so for code that isn't a tight loop the JIT mostly
 * runs out of cache.  This interpreter is the other trade: one small set
 * of handlers that stays in the I-cache, reading 68k code as data (which
 * is dense).
 *
 * So it's built for a small footprint first.  The top ten bits of an
 * opcode pick a handler (through a 1KB table, filled in as opcodes are
 * first seen); the handlers decode the rest at run time.  Register
 * operands are handled inline, memory operands by one shared routine.
 * The hottest operations have a handler per size, and the commonest
 * addressing modes are handled inline in them.  The flags are computed
 * directly in Musashi's format (the register file's), every size by the
 * same code, by working on values shifted to the top of a 32-bit word.  The pc stays in a register, as an offset
 * into the region of memory the code is in (RAM or ROM); routines that
 * read extension words take it and hand it back (with their result, as a
 * 64-bit value, i.e. in r0/r1).
 *
 * It runs on the JIT's register file, so the tiers (and Musashi, which
 * gets anything unusual: exceptions, privileged and BCD instructions...)
 * can hand over at any instruction boundary.
 */
#include <string.h>
#include "m68kjit.h"
#include "m68kjit_int.h"
#include "m68knative.h"

unsigned int cpu_read_byte(unsigned int address);
unsigned int cpu_read_word(unsigned int address);
unsigned int cpu_read_long(unsigned int address);
void cpu_write_word(unsigned int address, unsigned int value);
void cpu_write_long(unsigned int address, unsigned int value);

/* Testing builds (the lockstep verifier, which builds umac with its I/O
 * hook) report every RAM write to m68k_jit_write_observer
 */
#ifndef INTERP_OBSERVE
#ifdef UMAC_IO_HOOK
#define INTERP_OBSERVE  1
#else
#define INTERP_OBSERVE  0
#endif
#endif

#define PAGE_SHIFT      9               /* = the JIT's code pages */

#define NOINLINE        __attribute__((noinline))
#define INLINE          static inline __attribute__((always_inline))
#define LIKELY(x)       __builtin_expect(!!(x), 1)
#define UNLIKELY(x)     __builtin_expect(!!(x), 0)

m68k_interp_env_t m68k_interp_env;
uint32_t m68k_interp_slow_io;
#define ENV     m68k_interp_env

/* What the routines share (on the stack, i.e. in DTCM on the device) */
typedef struct {
        uint32_t *r;                    /* D0-D7, A0-A7 (the register file) */
        const uint8_t *cb;              /* host address of the code's region (RAM or ROM) */
        uint32_t pbase;                 /* 68k address of its start (pc = pbase + offset) */
        uint32_t climit;                /* ... offsets below this are in it (and fetchable) */
        uint8_t *ram;
        const uint8_t *cp;              /* the JIT's code-page map */
        uint32_t ramlim;                /* fast path: offsets below this (whole access in RAM) */
        int loc;                        /* read-modify-write operand: register 0-15, or -1: memory */
        uint32_t loc_a;                 /* ... at this address */
} ik_t;

/* A value and the pc (offset) after the words it took */
typedef uint64_t vp_t;
#define VP(v, pc)       ((uint64_t)(uint32_t)(pc) << 32 | (uint32_t)(v))
#define VP_V(x)         ((uint32_t)(x))
#define VP_PC(x)        ((uint32_t)((x) >> 32))

#define R(n)    (r[(n)])                /* D0-D7, A0-A7 (r = k->r) */
#define D(n)    R(n)
#define A(n)    R(8 + (n))

INLINE uint32_t be16(const uint8_t *p)
{
        uint16_t v;
        memcpy(&v, p, 2);
        return __builtin_bswap16(v);
}

INLINE uint32_t be32(const uint8_t *p)
{
        uint32_t v;
        memcpy(&v, p, 4);
        return __builtin_bswap32(v);
}

/* Instruction words, at pc offsets (ROM code never runs off its end) */
#define FETCH16(pc)     be16(k->cb + (pc))
#define FETCH32(pc)     be32(k->cb + (pc))
#define PC68(pc)        (k->pbase + (pc))       /* the 68k address */

/* Sizes: bytes 1, 2, 4; values are kept zero-extended */
INLINE uint32_t size_mask(int sz)
{
        return 0xffffffffu >> (32 - 8 * sz);
}

/* Point instruction fetch at pc's region (RAM, or the copy of the ROM
 * it's in); 0 if it's neither RAM nor ROM
 */
static int set_code(ik_t *k, uint32_t pc)
{
        uint32_t o = pc & 0xffffff;
        if (o + 2 <= ENV.ram_size) {
                k->cb = k->ram;
                k->pbase = pc - o;
                k->climit = ENV.ram_size - 1;
                return 1;
        }
        if ((o & 0xf00000) == 0x400000) {
                k->cb = ENV.rom;
                k->pbase = pc - (o & ENV.rom_mask);
                k->climit = ENV.rom_mask;
                return 1;
        }
        return 0;
}

/* -------------------------------------------------------------------- */
/* Memory                                                               */

static NOINLINE uint32_t rd_slow(uint32_t a, int sz)
{
        uint32_t o = a & 0xffffff;
        if ((o & 0xf00000) == 0x400000) {
                const uint8_t *p = ENV.rom + (o & ENV.rom_mask);
                if ((o & ENV.rom_mask) + 4 <= ENV.rom_mask + 1)
                        return sz == 1 ? p[0] : sz == 2 ? be16(p) : be32(p);
        }
        if (sz == 1)
                return cpu_read_byte(o);
#if INTERP_OBSERVE
        m68k_interp_slow_io++;
#endif
        return sz == 2 ? cpu_read_word(o) : cpu_read_long(o);
}

INLINE uint32_t mread(ik_t *k, uint32_t a, int sz)
{
        uint32_t o = a & 0xffffff;
        if (LIKELY(o < k->ramlim)) {
                const uint8_t *p = k->ram + o;
                return sz == 1 ? p[0] : sz == 2 ? be16(p) : be32(p);
        }
        return rd_slow(a, sz);
}

static NOINLINE uint32_t rd(ik_t *k, uint32_t a, int sz)
{
        return mread(k, a, sz);
}

static NOINLINE void wr_slow(uint32_t a, uint32_t v, int sz)
{
        uint32_t o = a & 0xffffff;
#if INTERP_OBSERVE
        if (sz != 1)
                m68k_interp_slow_io++;
#endif
        /* (RAM writes here note themselves, through umac's write hook) */
        if (sz == 1)
                m68k_jit_h_wr8(o, v & 0xff);
        else if (sz == 2)
                cpu_write_word(o, v & 0xffff);
        else
                cpu_write_long(o, v);
}

INLINE void mwrite(ik_t *k, uint32_t a, uint32_t v, int sz)
{
        uint32_t o = a & 0xffffff;
        if (LIKELY(o < k->ramlim)) {
#if INTERP_OBSERVE
                if (m68k_jit_write_observer)
                        m68k_jit_write_observer(o, sz);
#endif
                uint8_t *p = k->ram + o;
                if (sz == 1) {
                        p[0] = v;
                } else if (sz == 2) {
                        uint16_t h = __builtin_bswap16(v);
                        memcpy(p, &h, 2);
                } else {
                        uint32_t w = __builtin_bswap32(v);
                        memcpy(p, &w, 4);
                }
                /* Translated code there?  (Either page the write touches) */
                if (UNLIKELY(k->cp[o >> PAGE_SHIFT] | k->cp[(o + sz - 1) >> PAGE_SHIFT]))
                        m68k_jit_check_write(o, sz);
                return;
        }
        wr_slow(a, v, sz);
}

static NOINLINE void wr(ik_t *k, uint32_t a, uint32_t v, int sz)
{
        mwrite(k, a, v, sz);
}

/* MOVE.L to -(An): Musashi writes the two words, the low one first;
 * outside RAM that can differ from one long write
 */
static NOINLINE void wr_pd32(ik_t *k, uint32_t a, uint32_t v)
{
        if (LIKELY((a & 0xffffff) < k->ramlim)) {
                mwrite(k, a, v, 4);
                return;
        }
        mwrite(k, a + 2, v & 0xffff, 2);
        mwrite(k, a, v >> 16, 2);
}

/* -------------------------------------------------------------------- */
/* Effective addresses                                                  */

/* Validity classes: bit (mode) for modes 0-6, bit (7 + reg) for mode 7 */
#define C_DREG  0x001
#define C_AREG  0x002
#define C_AI    0x004
#define C_PI    0x008
#define C_PD    0x010
#define C_DI    0x020
#define C_IX    0x040
#define C_AW    0x080
#define C_AL    0x100
#define C_PCDI  0x200
#define C_PCIX  0x400
#define C_IMM   0x800
#define C_MALT  (C_AI | C_PI | C_PD | C_DI | C_IX | C_AW | C_AL)
#define C_DALT  (C_DREG | C_MALT)
#define C_DATA  (C_DALT | C_PCDI | C_PCIX | C_IMM)
#define C_CTRL  (C_AI | C_DI | C_IX | C_AW | C_AL | C_PCDI | C_PCIX)
#define C_CALT  (C_AI | C_DI | C_IX | C_AW | C_AL)

/* Is the 6-bit mode/register field ea in the class set `ok`? */
INLINE int ea_ok(uint32_t ea, uint32_t ok)
{
        uint32_t cls = ea < 0x38 ? ea >> 3 : 7 + (ea & 7);
        return (ok >> cls) & 1;
}

/* Address of a memory operand (modes 2-7, not immediate) at size sz;
 * applies the (An)+ and -(An) updates.  pc: its extension words.
 */
static NOINLINE vp_t ea_addr(ik_t *k, uint32_t pc, uint32_t ea, int sz)
{
        uint32_t *r = k->r, rn = ea & 7, *an = &A(rn), x, base;
        switch (ea >> 3) {
        case 2:
                return VP(*an, pc);
        case 3:
                x = *an;
                *an = x + (sz == 1 && rn == 7 ? 2 : sz);
                return VP(x, pc);
        case 4:
                x = *an - (sz == 1 && rn == 7 ? 2 : sz);
                *an = x;
                return VP(x, pc);
        case 5:
                return VP(*an + (int16_t)FETCH16(pc), pc + 2);
        case 6:
                base = *an;
                break;
        default:
                switch (rn) {
                case 0:
                        return VP((int16_t)FETCH16(pc), pc + 2);
                case 1:
                        return VP(FETCH32(pc), pc + 4);
                case 2:
                        return VP(PC68(pc) + (int16_t)FETCH16(pc), pc + 2);
                default:                /* 3: d8(PC,Xn) */
                        base = PC68(pc);
                        break;
                }
        }
        /* Brief extension word: the 68000 ignores the scale bits */
        x = FETCH16(pc);
        uint32_t xn = R(x >> 12);
        if (!(x & 0x800))
                xn = (uint32_t)(int16_t)xn;
        return VP(base + xn + (int8_t)x, pc + 2);
}

/* Operand value (zero-extended), any mode */
/* The commonest addressing modes inline ((An), (An)+, -(An), d16(An),
 * abs.W: the low-memory globals), else ea_addr.  Sets a (the address)
 * and pc.
 */
#define EA_ADDR(a, ea, sz) do {                                         \
                uint32_t *an_ = &A((ea) & 7), st_ = (sz) == 1 && ((ea) & 7) == 7 ? 2 : (sz); \
                switch ((ea) >> 3) {                                    \
                case 2: a = *an_; break;                                \
                case 3: a = *an_; *an_ = a + st_; break;                \
                case 4: a = *an_ - st_; *an_ = a; break;                \
                case 5: a = *an_ + (int16_t)FETCH16(pc); pc += 2; break; \
                case 7:                                                 \
                        if (((ea) & 7) == 0) {                          \
                                a = (int16_t)FETCH16(pc);               \
                                pc += 2;                                \
                                break;                                  \
                        }                                               \
                        __attribute__((fallthrough));                   \
                default: {                                              \
                        vp_t x_ = ea_addr(k, pc, (ea), (sz));           \
                        a = VP_V(x_);                                   \
                        pc = VP_PC(x_);                                 \
                }                                                       \
                }                                                       \
        } while (0)

static NOINLINE vp_t ea_read(ik_t *k, uint32_t pc, uint32_t ea, int sz)
{
        uint32_t *r = k->r, a;
        if (ea < 0x10)
                return VP(R(ea) & size_mask(sz), pc);
        if (ea == 0x3c) {
                if (sz == 4)
                        return VP(FETCH32(pc), pc + 4);
                return VP(FETCH16(pc) & size_mask(sz), pc + 2);
        }
        EA_ADDR(a, ea, sz);
        return VP(mread(k, a, sz), pc);
}

/* Write a destination-only operand (Dn or memory); easz = ea | size << 8.
 * Returns the pc past its words.
 */
static NOINLINE uint32_t ea_write(ik_t *k, uint32_t pc, uint32_t easz, uint32_t v)
{
        uint32_t *r = k->r, ea = easz & 0x3f, a;
        int sz = easz >> 8;
        if (ea < 8) {
                uint32_t m = size_mask(sz), *p = &R(ea);
                *p = (*p & ~m) | (v & m);
                return pc;
        }
        EA_ADDR(a, ea, sz);
        mwrite(k, a, v, sz);
        return pc;
}

/* Read-modify-write operand: read it, remembering where it was */
static NOINLINE vp_t rmw_read(ik_t *k, uint32_t pc, uint32_t ea, int sz)
{
        uint32_t *r = k->r;
        if (ea < 0x10) {
                k->loc = ea;
                return VP(R(ea) & size_mask(sz), pc);
        }
        k->loc = -1;
        vp_t x = ea_addr(k, pc, ea, sz);
        k->loc_a = VP_V(x);
        return VP(mread(k, VP_V(x), sz), VP_PC(x));
}

static NOINLINE void rmw_write(ik_t *k, uint32_t v, int sz)
{
        if (k->loc >= 0) {
                uint32_t m = size_mask(sz), *p = &k->r[k->loc];
                *p = (*p & ~m) | (v & m);
                return;
        }
        mwrite(k, k->loc_a, v, sz);
}

static void push32(ik_t *k, uint32_t v)
{
        uint32_t sp = k->r[15] - 4;
        k->r[15] = sp;
        wr(k, sp, v, 4);
}

/* -------------------------------------------------------------------- */
/* Flags (Musashi's format: N, V bit 7; C, X bit 8; Z: not_z == 0)      */

/* N and Z of v (at size sz), V = C = 0 */
INLINE void flags_logic(jregs_t *j, uint32_t v, int sz)
{
        uint32_t t = v << (32 - 8 * sz);
        j->n = t >> 24;
        j->not_z = t;
        j->v = 0;
        j->c = 0;
}

/* dst + src / dst - src at size sz (values zero-extended), flags set
 * (X too if setx) -> result
 */
INLINE uint32_t add_f(jregs_t *j, uint32_t dst, uint32_t src, int sz, int setx)
{
        int sh = 32 - 8 * sz;
        uint32_t a = src << sh, b = dst << sh, r = a + b, cf = (r < a) << 8;
        j->n = r >> 24;
        j->not_z = r;
        j->v = ((a ^ r) & (b ^ r)) >> 24;       /* (only bit 7 counts) */
        j->c = cf;
        if (setx)
                j->x = cf;
        return r >> sh;
}

INLINE uint32_t sub_f(jregs_t *j, uint32_t dst, uint32_t src, int sz, int setx)
{
        int sh = 32 - 8 * sz;
        uint32_t a = src << sh, b = dst << sh, r = b - a, cf = (a > b) << 8;
        j->n = r >> 24;
        j->not_z = r;
        j->v = ((a ^ b) & (r ^ b)) >> 24;
        j->c = cf;
        if (setx)
                j->x = cf;
        return r >> sh;
}

enum { AR_ADD, AR_SUB, AR_CMP, AR_ADDX, AR_SUBX };

/* dst OP src at size sz -> result (flags set; X too, except for CMP) */
static NOINLINE uint32_t arith(jregs_t *j, int op, uint32_t dst, uint32_t src, int sz)
{
        if (op == AR_ADD)
                return add_f(j, dst, src, sz, 1);
        if (op == AR_SUB)
                return sub_f(j, dst, src, sz, 1);
        if (op == AR_CMP)
                return sub_f(j, dst, src, sz, 0);
        /* ADDX / SUBX: with X in, and they only ever clear Z */
        int sh = 32 - 8 * sz;
        uint32_t a = src << sh, b = dst << sh, r, cf, v;
        uint64_t x = (uint64_t)((j->x >> 8) & 1) << sh;
        if (op == AR_ADDX) {
                uint64_t t = (uint64_t)a + b + x;
                r = (uint32_t)t;
                cf = (uint32_t)(t >> 32);
                v = (a ^ r) & (b ^ r);
        } else {
                uint64_t t = (uint64_t)b - a - x;
                r = (uint32_t)t;
                cf = (uint32_t)(t >> 32) & 1;
                v = (a ^ b) & (r ^ b);
        }
        j->n = r >> 24;
        j->v = v >> 24;
        j->c = j->x = cf << 8;
        j->not_z |= r;
        return r >> sh;
}

/* 68k condition cc from the flags: a truth table over N Z V C */
static const uint16_t cc_table[16] = {
        0xffff, 0x0000, 0x0505, 0xfafa, 0x5555, 0xaaaa, 0x0f0f, 0xf0f0,
        0x3333, 0xcccc, 0x00ff, 0xff00, 0xcc33, 0x33cc, 0x0c03, 0xf3fc,
};

INLINE int cond(const jregs_t *j, int cc)
{
        uint32_t f = ((j->n >> 4) & 8) | (j->not_z ? 0 : 4) | ((j->v >> 6) & 2) | ((j->c >> 8) & 1);
        return (cc_table[cc] >> f) & 1;
}

/* Conditions straight from the flags */
#define CC_C(j)         ((j)->c & 0x100)
#define CC_Z(j)         (!(j)->not_z)
#define CC_N(j)         ((j)->n & 0x80)
#define CC_V(j)         ((j)->v & 0x80)
#define CC_NV(j)        (((j)->n ^ (j)->v) & 0x80)      /* N != V */

static void set_ccr(jregs_t *j, uint32_t v)
{
        j->x = (v & 0x10) << 4;
        j->n = (v & 8) << 4;
        j->not_z = !(v & 4);
        j->v = (v & 2) << 6;
        j->c = (v & 1) << 8;
}

/* -------------------------------------------------------------------- */
/* Shifts and rotates: type 0 AS, 1 LS, 2 ROX, 3 RO; count 0-63         */

static NOINLINE uint32_t shift(jregs_t *j, int type, int left, uint32_t v, uint32_t cnt, int sz)
{
        int w = 8 * sz;
        uint32_t m = size_mask(sz), r, cf = 0;
        v &= m;
        j->v = 0;
        if (cnt == 0) {
                r = v;
                cf = type == 2 ? (j->x >> 8) & 1 : 0;   /* ROX: C = X */
        } else if (type == 2) {
                /* ROX: rotate the w+1 bits X:v */
                uint64_t t = (uint64_t)v | (uint64_t)((j->x >> 8) & 1) << w;
                uint32_t n = cnt % (w + 1);
                if (n) {
                        if (left)
                                t = (t << n) | (t >> (w + 1 - n));
                        else
                                t = (t >> n) | (t << (w + 1 - n));
                }
                r = (uint32_t)t & m;
                cf = (uint32_t)(t >> w) & 1;
                j->x = cf << 8;
        } else if (type == 3) {
                uint32_t n = cnt & (w - 1);
                r = left ? (v << n | v >> ((w - n) & (w - 1))) & m : (v >> n | v << ((w - n) & (w - 1))) & m;
                cf = left ? r & 1 : (r >> (w - 1)) & 1;
        } else {
                if (left) {
                        uint64_t t = (uint64_t)v << cnt;        /* cnt <= 63 */
                        r = (uint32_t)t & m;
                        cf = (uint32_t)(t >> w) & 1;
                        if (type == 0) {
                                /* ASL: V if the sign changed at any point */
                                if (cnt >= (uint32_t)w) {
                                        j->v = v ? 0x80 : 0;
                                } else {
                                        uint32_t top = v >> (w - 1 - cnt);  /* the top cnt+1 bits */
                                        j->v = (top != 0 && top != (2u << cnt) - 1) ? 0x80 : 0;
                                }
                        }
                } else {
                        int64_t s = type == 0 ? (int64_t)((int32_t)(v << (32 - w))) >> (32 - w) : (int64_t)v;
                        r = (uint32_t)(s >> cnt) & m;
                        cf = (uint32_t)(s >> (cnt - 1)) & 1;
                }
                j->x = cf << 8;
        }
        j->c = cf << 8;
        uint32_t t = r << (32 - w);
        j->n = t >> 24;
        j->not_z = t;
        return r;
}

/* -------------------------------------------------------------------- */
/* Control flow                                                         */

/* Carry on at pc: runs any native routine there; in tiered modes, hot
 * code there goes to the JIT.  how: J_ENTRY (where a run starts: the
 * dispatcher has just decided to interpret it), J_JUMP (a forward branch,
 * jump, call or return) or J_BACK (a backward branch: a loop).  Returns 0
 * to carry on (k set up for pc, j->pc = pc), else an IX_ reason (j->pc
 * set).
 */
enum { J_ENTRY, J_JUMP, J_BACK };
static NOINLINE int jump_slow(ik_t *k, jregs_t *j, uint32_t pc, int how)
{
        while (UNLIKELY((ENV.natbits[(pc >> 4) & 31] >> ((pc >> 1) & 7)) & 1) &&
               j->budget > 0) {
                int idx = m68k_native_lookup(pc);
                if (idx < 0)
                        break;                  /* (just a hash collision) */
                j->pc = pc;
                pc = m68k_jit_h_native(idx, pc);
                j->budget -= j->native_n;
                if (how == J_BACK)
                        how = J_JUMP;
        }
        j->pc = pc;
        if ((how == J_BACK && ENV.tier == 2) || (how && ENV.tier == 3)) {
                uint8_t *h = &ENV.heat[m68k_heat_index(pc)];
                if (*h >= ENV.hot)
                        return IX_HOT;
                (*h)++;
        }
        return set_code(k, pc) ? 0 : IX_NOFETCH;
}

/* -------------------------------------------------------------------- */
/* Decoding: the top ten bits of the opcode -> handler                  */

/* Hot operations get a handler per size (_B, _W, _L, consecutive) */
enum {
        H_NEW,                  /* not classified yet */
        H_MUSASHI,              /* (none of the below) */
        H_MOVE_D_B, H_MOVE_D_W, H_MOVE_D_L,     /* MOVE to Dn */
        H_MOVE_A_W, H_MOVE_A_L,                 /* MOVEA */
        H_MOVE_M_B, H_MOVE_M_W, H_MOVE_M_L,     /* MOVE to memory */
        H_OR_D_B, H_OR_D_W, H_OR_D_L,           /* <ea> OP Dn */
        H_AND_D_B, H_AND_D_W, H_AND_D_L,
        H_SUB_D_B, H_SUB_D_W, H_SUB_D_L,
        H_ADD_D_B, H_ADD_D_W, H_ADD_D_L,
        H_CMP_D_B, H_CMP_D_W, H_CMP_D_L,
        H_ADDQ_B, H_ADDQ_W, H_ADDQ_L,
        H_SUBQ_B, H_SUBQ_W, H_SUBQ_L,
        H_TST_B, H_TST_W, H_TST_L,
        H_CLR_B, H_CLR_W, H_CLR_L,
        H_ADDA_W, H_ADDA_L, H_SUBA_W, H_SUBA_L, H_CMPA_W, H_CMPA_L,
        H_BRA, H_BSR, H_BHI, H_BLS, H_BCC, H_BCS, H_BNE, H_BEQ,        /* Bcc, by condition */
        H_BVC, H_BVS, H_BPL, H_BMI, H_BGE, H_BLT, H_BGT, H_BLE,
        H_MOVEQ,
        H_SCC,                  /* (incl. DBcc) */
        H_LEA, H_4E4,           /* $4e40-$4e7f: LINK UNLK RTS NOP RTE RTR ... */
        H_JSR, H_JMP, H_MOVEM, H_EXT, H_NEG, H_NOT,
        H_MOVEFSR, H_MOVETCCR, H_MOVETSR, H_SWAPPEA,
        H_ALINE,
        H_OR_M, H_AND_M, H_SUB_M, H_ADD_M, H_EOR_M,     /* Dn,<ea> (+ pair forms) */
        H_MUL, H_DIV,
        H_BIT, H_IMM,
        H_SHIFT_R, H_SHIFT_M,
        H_COUNT
};

static uint8_t htab[1024];

static NOINLINE int classify(uint32_t hi)       /* hi = opcode >> 6 */
{
        uint32_t line = hi >> 6, opm = hi & 7, sz = hi & 3;
        switch (line) {
        case 0x0:
                if ((hi & 4) || (hi & 0x38) == 0x20)
                        return H_BIT;
                return H_IMM;
        case 0x1: case 0x2: case 0x3: {
                int s = line == 1 ? 0 : line == 3 ? 1 : 2;
                if (opm == 1)
                        return s ? H_MOVE_A_W + s - 1 : H_MUSASHI;
                return (opm == 0 ? H_MOVE_D_B : H_MOVE_M_B) + s;
        }
        case 0x4:
                if ((hi & 7) == 7)
                        return H_LEA;
                switch (hi & 0x3f) {
                case 0x00: case 0x01: case 0x02: case 0x10: case 0x11: case 0x12: return H_NEG;
                case 0x03: return H_MOVEFSR;
                case 0x08: case 0x09: case 0x0a: return H_CLR_B + sz;
                case 0x13: return H_MOVETCCR;
                case 0x18: case 0x19: case 0x1a: return H_NOT;
                case 0x1b: return H_MOVETSR;
                case 0x21: return H_SWAPPEA;
                case 0x22: case 0x23: return H_EXT;     /* (and MOVEM to memory) */
                case 0x28: case 0x29: case 0x2a: return H_TST_B + sz;
                case 0x32: case 0x33: return H_MOVEM;
                case 0x39: return H_4E4;
                case 0x3a: return H_JSR;
                case 0x3b: return H_JMP;
                }
                return H_MUSASHI;
        case 0x5:
                return sz == 3 ? H_SCC : ((hi & 4) ? H_SUBQ_B : H_ADDQ_B) + sz;
        case 0x6:
                return H_BRA + ((hi >> 2) & 15);
        case 0x7:
                return (hi & 4) ? H_MUSASHI : H_MOVEQ;
        case 0x8: case 0x9: case 0xb: case 0xc: case 0xd:
                if (opm == 3 || opm == 7) {
                        if (line == 0x8)
                                return H_DIV;
                        if (line == 0xc)
                                return H_MUL;
                        return (line == 0x9 ? H_SUBA_W : line == 0xd ? H_ADDA_W : H_CMPA_W) + (opm == 7);
                }
                if (opm < 3) {
                        static const uint8_t d[] = { H_OR_D_B, H_SUB_D_B, 0, H_CMP_D_B, H_AND_D_B, H_ADD_D_B };
                        return d[line - 8] + opm;
                } else {
                        static const uint8_t m[] = { H_OR_M, H_SUB_M, 0, H_EOR_M, H_AND_M, H_ADD_M };
                        return m[line - 8];
                }
        case 0xa:
                return H_ALINE;
        case 0xe:
                return sz == 3 ? H_SHIFT_M : H_SHIFT_R;
        }
        return H_MUSASHI;
}

/* -------------------------------------------------------------------- */
/* The interpreter                                                      */

/* Instruction-field shorthands */
#define OP_EA   (op & 0x3f)
#define OP_RY   (op & 7)
#define OP_RX   ((op >> 9) & 7)
#define OP_SZ   ((op >> 6) & 3)         /* 0 B, 1 W, 2 L, 3 (other) */
#define SZ_BYTES(s)     (1 << (s))      /* for s = 0..2 */

/* Source operand into v (register operands inline) */
#define SRC(v, ea, sz)  do {                                            \
                if ((ea) < 0x10) {                                      \
                        v = R(ea) & size_mask(sz);                      \
                } else {                                                \
                        vp_t x_ = ea_read(k, pc, (ea), (sz));           \
                        v = VP_V(x_);                                   \
                        pc = VP_PC(x_);                                 \
                }                                                       \
        } while (0)

/* Leave the loop, with j->pc set (budget saved) */
#define EXIT(why)       do { j->budget = budget; return (why); } while (0)

/* Take a branch, jump, call or return to (68k address) t.  The fast
 * case inline: no native routine there, in the same region as now, and
 * no tiering to do (tier 2 watches backward branches, tier 3 everything)
 */
#define JUMP(t, back)   do {                                            \
                uint32_t t_ = (t), o_ = t_ - k->pbase;                  \
                if (LIKELY(!((ENV.natbits[(t_ >> 4) & 31] >> ((t_ >> 1) & 7)) & 1) && \
                           o_ < k->climit && !(((back) && ENV.tier == 2) || ENV.tier == 3))) { \
                        pc = o_;                                        \
                } else {                                                \
                        j->budget = budget;                             \
                        int w_ = jump_slow(k, j, t_, (back) ? J_BACK : J_JUMP); \
                        budget = j->budget;                             \
                        if (w_)                                         \
                                return w_;                              \
                        pc = j->pc - k->pbase;                          \
                }                                                       \
        } while (0)

/* A forward branch: no native routine to look for (they're reached by
 * calls, returns and loops), nor heat except in tier 3
 */
#define JUMP_FWD(t)     do {                                            \
                uint32_t t_ = (t), o_ = t_ - k->pbase;                  \
                if (LIKELY(o_ < k->climit && ENV.tier != 3)) {        \
                        pc = o_;                                        \
                } else {                                                \
                        j->budget = budget;                             \
                        int w_ = jump_slow(k, j, t_, J_JUMP);           \
                        budget = j->budget;                             \
                        if (w_)                                         \
                                return w_;                              \
                        pc = j->pc - k->pbase;                          \
                }                                                       \
        } while (0)

/* Source operand into v, memory operands inline too (for MOVE) */
#define SRC_INL(v, ea, sz)  do {                                        \
                if ((ea) < 0x10) {                                      \
                        v = R(ea) & size_mask(sz);                      \
                } else if ((ea) == 0x3c) {                              \
                        if ((sz) == 4) {                                \
                                v = FETCH32(pc);                        \
                                pc += 4;                                \
                        } else {                                        \
                                v = FETCH16(pc) & size_mask(sz);        \
                                pc += 2;                                \
                        }                                               \
                } else {                                                \
                        uint32_t a_;                                    \
                        EA_ADDR(a_, (ea), (sz));                        \
                        v = mread(k, a_, (sz));                         \
                }                                                       \
        } while (0)

/* Size-specialised handler bodies (SZ a constant) */

#define MOVE_D(SZ) {                                                    \
                ea = OP_EA;                                             \
                if (ea > 0x3c || (SZ == 1 && (ea & 0x38) == 8))         \
                        goto musashi;                                   \
                SRC_INL(v, ea, SZ);                                         \
                uint32_t *d_ = &D(OP_RX);                               \
                *d_ = (*d_ & ~size_mask(SZ)) | v;                       \
                flags_logic(j, v, SZ);                                  \
                continue;                                               \
        }

#define MOVE_A(SZ) {                                                    \
                ea = OP_EA;                                             \
                if (ea > 0x3c)                                          \
                        goto musashi;                                   \
                SRC_INL(v, ea, SZ);                                         \
                A(OP_RX) = SZ == 2 ? (uint32_t)(int16_t)v : v;          \
                continue;                                               \
        }

#define MOVE_M(SZ) {                                                    \
                ea = OP_EA;                                             \
                uint32_t d_ = ((op >> 3) & 0x38) | OP_RX;               \
                if (ea > 0x3c || (SZ == 1 && (ea & 0x38) == 8) || d_ > 0x39) \
                        goto musashi;                                   \
                SRC_INL(v, ea, SZ);                                     \
                if (d_ < 8) {                                           \
                        uint32_t *dn_ = &D(d_);                         \
                        *dn_ = (*dn_ & ~size_mask(SZ)) | v;             \
                } else {                                                \
                        EA_ADDR(a, d_, SZ);                             \
                        if (SZ == 4 && (d_ >> 3) == 4)                  \
                                wr_pd32(k, a, v);                       \
                        else                                            \
                                mwrite(k, a, v, SZ);                    \
                }                                                       \
                flags_logic(j, v, SZ);                                  \
                continue;                                               \
        }

/* <ea> OP Dn -> Dn, OP: 0 OR, 1 AND, 2 SUB, 3 ADD, 4 CMP */
#define ALU_D(OP, SZ) {                                                 \
                ea = OP_EA;                                             \
                if (ea > 0x3c || ((ea & 0x38) == 8 && (SZ == 1 || OP <= 1))) \
                        goto musashi;                                   \
                if (OP == 4)                                            \
                        SRC_INL(s, ea, SZ);                             \
                else                                                    \
                        SRC(s, ea, SZ);                                 \
                uint32_t *d_ = &D(OP_RX), m_ = size_mask(SZ), dv_ = *d_ & m_; \
                if (OP == 0) {                                          \
                        v = dv_ | s;                                    \
                        flags_logic(j, v, SZ);                          \
                } else if (OP == 1) {                                   \
                        v = dv_ & s;                                    \
                        flags_logic(j, v, SZ);                          \
                } else if (OP == 2) {                                   \
                        v = sub_f(j, dv_, s, SZ, 1);                    \
                } else if (OP == 3) {                                   \
                        v = add_f(j, dv_, s, SZ, 1);                    \
                } else {                                                \
                        sub_f(j, dv_, s, SZ, 0);                        \
                        continue;                                       \
                }                                                       \
                *d_ = (*d_ & ~m_) | v;                                  \
                continue;                                               \
        }

/* ADDQ / SUBQ */
#define QUICK(SUB, SZ) {                                                \
                s = ((OP_RX - 1) & 7) + 1;                              \
                ea = OP_EA;                                             \
                if (ea < 8) {                                           \
                        uint32_t *d_ = &D(ea), m_ = size_mask(SZ);      \
                        v = SUB ? sub_f(j, *d_ & m_, s, SZ, 1) : add_f(j, *d_ & m_, s, SZ, 1); \
                        *d_ = (*d_ & ~m_) | v;                          \
                        continue;                                       \
                }                                                       \
                if (ea < 0x10) {        /* An: all 32 bits, no flags */ \
                        if (SZ == 1)                                    \
                                goto musashi;                           \
                        R(ea) += SUB ? -s : s;                          \
                        continue;                                       \
                }                                                       \
                if (ea > 0x39)                                          \
                        goto musashi;                                   \
                vp_t x_ = rmw_read(k, pc, ea, SZ);                      \
                pc = VP_PC(x_);                                         \
                rmw_write(k, arith(j, SUB ? AR_SUB : AR_ADD, VP_V(x_), s, SZ), SZ); \
                continue;                                               \
        }

#define TST(SZ) {                                                       \
                ea = OP_EA;                                             \
                if (ea > 0x39 || (ea & 0x38) == 8)                      \
                        goto musashi;                                   \
                SRC(v, ea, SZ);                                         \
                flags_logic(j, v, SZ);                                  \
                continue;                                               \
        }

#define CLR(SZ) {                                                       \
                ea = OP_EA;                                             \
                if (ea > 0x39 || (ea & 0x38) == 8)                      \
                        goto musashi;                                   \
                pc = ea_write(k, pc, SZ << 8 | ea, 0);                  \
                flags_logic(j, 0, 4);                                   \
                continue;                                               \
        }

/* ADDA / SUBA / CMPA, OP: 0 ADD, 1 SUB, 2 CMP */
#define ALU_A(OP, SZ) {                                                 \
                ea = OP_EA;                                             \
                if (ea > 0x3c)                                          \
                        goto musashi;                                   \
                SRC(v, ea, SZ);                                         \
                if (SZ == 2)                                            \
                        v = (uint32_t)(int16_t)v;                       \
                uint32_t *a_ = &A(OP_RX);                               \
                if (OP == 2)                                            \
                        sub_f(j, *a_, v, 4, 0);                         \
                else if (OP == 1)                                       \
                        *a_ -= v;                                       \
                else                                                    \
                        *a_ += v;                                       \
                continue;                                               \
        }

int m68k_interp_run(jregs_t *j)
{
        ik_t kk, *k = &kk;
        const uint8_t *ht = htab;
        /* (Hide where these point: otherwise the compiler keeps copies of
         * k's fields and writes them all back around every call)
         */
        __asm__("" : "+r"(k), "+r"(ht));
        uint32_t *r = j->dar;
        k->r = r;
        k->ram = ENV.ram;
        k->cp = ENV.codepage;
        k->ramlim = ENV.ram_size - 3;
        int why = jump_slow(k, j, j->pc, J_ENTRY);
        if (why)
                return why;
        uint32_t pc = j->pc - k->pbase;         /* (an offset: see PC68) */
        int32_t budget = j->budget;

        while (--budget >= 0) {
                uint32_t ipc = pc, op = FETCH16(pc), ea, v, s, a;
                int sz;
                pc += 2;
                switch (ht[op >> 6]) {
                case H_NEW:
                        htab[op >> 6] = classify(op >> 6);
                        pc = ipc;
                        budget++;
                        continue;

                case H_MOVE_D_B: MOVE_D(1)
                case H_MOVE_D_W: MOVE_D(2)
                case H_MOVE_D_L: MOVE_D(4)
                case H_MOVE_A_W: MOVE_A(2)
                case H_MOVE_A_L: MOVE_A(4)
                case H_MOVE_M_B: MOVE_M(1)
                case H_MOVE_M_W: MOVE_M(2)
                case H_MOVE_M_L: MOVE_M(4)
                case H_OR_D_B: ALU_D(0, 1)
                case H_OR_D_W: ALU_D(0, 2)
                case H_OR_D_L: ALU_D(0, 4)
                case H_AND_D_B: ALU_D(1, 1)
                case H_AND_D_W: ALU_D(1, 2)
                case H_AND_D_L: ALU_D(1, 4)
                case H_SUB_D_B: ALU_D(2, 1)
                case H_SUB_D_W: ALU_D(2, 2)
                case H_SUB_D_L: ALU_D(2, 4)
                case H_ADD_D_B: ALU_D(3, 1)
                case H_ADD_D_W: ALU_D(3, 2)
                case H_ADD_D_L: ALU_D(3, 4)
                case H_CMP_D_B: ALU_D(4, 1)
                case H_CMP_D_W: ALU_D(4, 2)
                case H_CMP_D_L: ALU_D(4, 4)
                case H_ADDQ_B: QUICK(0, 1)
                case H_ADDQ_W: QUICK(0, 2)
                case H_ADDQ_L: QUICK(0, 4)
                case H_SUBQ_B: QUICK(1, 1)
                case H_SUBQ_W: QUICK(1, 2)
                case H_SUBQ_L: QUICK(1, 4)
                case H_TST_B: TST(1)
                case H_TST_W: TST(2)
                case H_TST_L: TST(4)
                case H_CLR_B: CLR(1)
                case H_CLR_W: CLR(2)
                case H_CLR_L: CLR(4)
                case H_ADDA_W: ALU_A(0, 2)
                case H_ADDA_L: ALU_A(0, 4)
                case H_SUBA_W: ALU_A(1, 2)
                case H_SUBA_L: ALU_A(1, 4)
                case H_CMPA_W: ALU_A(2, 2)
                case H_CMPA_L: ALU_A(2, 4)

                /* Bcc: the test, then the shared taken / not-taken paths */
                case H_BRA: goto bcc_taken;
                case H_BHI: if (!CC_C(j) && !CC_Z(j)) goto bcc_taken; goto bcc_not;
                case H_BLS: if (CC_C(j) || CC_Z(j)) goto bcc_taken; goto bcc_not;
                case H_BCC: if (!CC_C(j)) goto bcc_taken; goto bcc_not;
                case H_BCS: if (CC_C(j)) goto bcc_taken; goto bcc_not;
                case H_BNE: if (!CC_Z(j)) goto bcc_taken; goto bcc_not;
                case H_BEQ: if (CC_Z(j)) goto bcc_taken; goto bcc_not;
                case H_BVC: if (!CC_V(j)) goto bcc_taken; goto bcc_not;
                case H_BVS: if (CC_V(j)) goto bcc_taken; goto bcc_not;
                case H_BPL: if (!CC_N(j)) goto bcc_taken; goto bcc_not;
                case H_BMI: if (CC_N(j)) goto bcc_taken; goto bcc_not;
                case H_BGE: if (!CC_NV(j)) goto bcc_taken; goto bcc_not;
                case H_BLT: if (CC_NV(j)) goto bcc_taken; goto bcc_not;
                case H_BGT: if (!CC_NV(j) && !CC_Z(j)) goto bcc_taken; goto bcc_not;
                case H_BLE: if (CC_NV(j) || CC_Z(j)) goto bcc_taken; goto bcc_not;
                bcc_not:
                        if ((op & 0xff) == 0)
                                pc += 2;
                        else if ((op & 0xff) == 0xff)
                                goto musashi;           /* (68020 long branch) */
                        continue;
                bcc_taken: {
                        int32_t disp = (int8_t)op;
                        uint32_t base = pc;
                        if (disp == 0)
                                disp = (int16_t)FETCH16(pc);
                        else if (disp == -1)
                                goto musashi;
                        if (disp >= 0)
                                JUMP_FWD(PC68(base) + disp);
                        else
                                JUMP(PC68(base) + disp, 1);
                        continue;
                }

                case H_BSR: {
                        int32_t disp = (int8_t)op;
                        uint32_t base = pc;
                        if (disp == 0) {
                                disp = (int16_t)FETCH16(pc);
                                pc += 2;
                        } else if (disp == -1) {
                                goto musashi;
                        }
                        push32(k, PC68(pc));
                        JUMP(PC68(base) + disp, 0);
                        continue;
                }

                case H_MOVEQ:
                        v = (uint32_t)(int8_t)op;
                        D(OP_RX) = v;
                        flags_logic(j, v, 4);
                        continue;

                case H_SCC: {
                        int ccd = (op >> 8) & 15;
                        if ((op & 0x38) == 0x08) {      /* DBcc */
                                if (ccd != 1 && cond(j, ccd)) {
                                        pc += 2;
                                        continue;
                                }
                                uint32_t *dn = &D(OP_RY);
                                uint32_t cnt = (*dn - 1) & 0xffff;
                                *dn = (*dn & 0xffff0000) | cnt;
                                if (cnt == 0xffff) {
                                        pc += 2;
                                        continue;
                                }
                                int32_t disp = (int16_t)FETCH16(pc);
                                if (disp >= 0)
                                        JUMP_FWD(PC68(pc) + disp);
                                else
                                        JUMP(PC68(pc) + disp, 1);
                                continue;
                        }
                        if (!ea_ok(OP_EA, C_DALT))
                                goto musashi;
                        pc = ea_write(k, pc, 1 << 8 | OP_EA, cond(j, ccd) ? 0xff : 0);
                        continue;
                }

                case H_LEA: {
                        if (!ea_ok(OP_EA, C_CTRL))
                                goto musashi;
                        vp_t x = ea_addr(k, pc, OP_EA, 4);
                        A(OP_RX) = VP_V(x);
                        pc = VP_PC(x);
                        continue;
                }

                case H_4E4:
                        switch (op & 0x3f) {
                        case 0x35:                                      /* RTS */
                                a = A(7);
                                A(7) = a + 4;
                                JUMP(mread(k, a, 4), 0);
                                continue;
                        case 0x10: case 0x11: case 0x12: case 0x13:     /* LINK */
                        case 0x14: case 0x15: case 0x16: case 0x17: {
                                int32_t d = (int16_t)FETCH16(pc);
                                uint32_t sp = A(7) - 4;
                                pc += 2;
                                A(7) = sp;
                                wr(k, sp, A(OP_RY), 4);
                                A(OP_RY) = sp;
                                A(7) += d;
                                continue;
                        }
                        case 0x18: case 0x19: case 0x1a: case 0x1b:     /* UNLK */
                        case 0x1c: case 0x1d: case 0x1e: case 0x1f:
                                a = A(OP_RY);
                                A(7) = a + 4;
                                A(OP_RY) = rd(k, a, 4);
                                continue;
                        case 0x31:                                      /* NOP */
                                continue;
                        case 0x33:                                      /* RTE */
                                j->pc = m68k_jit_h_rte(PC68(ipc));
                                EXIT(IX_SYNC);
                        case 0x37:                                      /* RTR */
                                a = A(7);
                                A(7) = a + 6;
                                set_ccr(j, rd(k, a, 2));
                                JUMP(rd(k, a + 2, 4), 0);
                                continue;
                        }
                        goto musashi;

                case H_JSR: case H_JMP: {
                        if (!ea_ok(OP_EA, C_CTRL))
                                goto musashi;
                        vp_t x = ea_addr(k, pc, OP_EA, 4);
                        pc = VP_PC(x);
                        if (!(op & 0x40))
                                push32(k, PC68(pc));
                        JUMP(VP_V(x), 0);
                        continue;
                }

                case H_EXT:
                        if (OP_EA < 8) {
                                v = D(OP_RY);
                                if (op & 0x40) {
                                        v = (uint32_t)(int16_t)v;
                                        D(OP_RY) = v;
                                        flags_logic(j, v, 4);
                                } else {
                                        v = (uint32_t)(int8_t)v & 0xffff;
                                        D(OP_RY) = (D(OP_RY) & 0xffff0000) | v;
                                        flags_logic(j, v, 2);
                                }
                                continue;
                        }
                        /* fall through: MOVEM to memory */
                case H_MOVEM: {
                        /* The register mask comes before the operand's words */
                        uint32_t spec = (op & 0x40 ? MV_LONG : 0) | (uint32_t)OP_RY << 20;
                        if (!ea_ok(OP_EA, op & 0x400 ? C_CTRL | C_PI : C_CALT | C_PD))
                                goto musashi;
                        spec |= FETCH16(pc);
                        pc += 2;
                        if (op & 0x400)
                                spec |= MV_TOREGS;
                        if ((OP_EA >> 3) == (op & 0x400 ? 3 : 4)) {
                                spec |= MV_AUTO;
                                a = A(OP_RY);
                        } else {
                                vp_t x = ea_addr(k, pc, OP_EA, op & 0x40 ? 4 : 2);
                                a = VP_V(x);
                                pc = VP_PC(x);
                        }
                        m68k_jit_h_movem(a, spec);
                        continue;
                }

                case H_NEG: case H_NOT: {
                        if (!ea_ok(OP_EA, C_DALT))
                                goto musashi;
                        sz = SZ_BYTES(OP_SZ);
                        vp_t x = rmw_read(k, pc, OP_EA, sz);
                        pc = VP_PC(x);
                        if (ht[op >> 6] == H_NOT) {
                                v = ~VP_V(x) & size_mask(sz);
                                flags_logic(j, v, sz);
                        } else {                        /* NEG / NEGX */
                                v = arith(j, op & 0x400 ? AR_SUB : AR_SUBX, 0, VP_V(x), sz);
                        }
                        rmw_write(k, v, sz);
                        continue;
                }

                case H_MOVEFSR:
                        if (!ea_ok(OP_EA, C_DALT))
                                goto musashi;
                        if (OP_EA < 8) {
                                D(OP_RY) = (D(OP_RY) & 0xffff0000) | m68k_jit_get_sr();
                        } else {
                                vp_t x = ea_addr(k, pc, OP_EA, 2);
                                pc = VP_PC(x);
                                wr(k, VP_V(x), m68k_jit_get_sr(), 2);
                        }
                        continue;

                case H_MOVETCCR:
                        if (!ea_ok(OP_EA, C_DATA))
                                goto musashi;
                        SRC(v, OP_EA, 2);
                        set_ccr(j, v);
                        continue;

                case H_MOVETSR:
                        if (!ea_ok(OP_EA, C_DATA) || !m68k_jit_h_super())
                                goto musashi;
                        SRC(v, OP_EA, 2);
                        j->pc = m68k_jit_set_sr_then(v, PC68(pc));
                        EXIT(IX_SYNC);

                case H_SWAPPEA:
                        if (OP_EA < 8) {                /* SWAP */
                                v = D(OP_RY);
                                v = v >> 16 | v << 16;
                                D(OP_RY) = v;
                                flags_logic(j, v, 4);
                                continue;
                        }
                        if (!ea_ok(OP_EA, C_CTRL)) {    /* PEA */
                                goto musashi;
                        } else {
                                vp_t x = ea_addr(k, pc, OP_EA, 4);
                                pc = VP_PC(x);
                                push32(k, VP_V(x));
                        }
                        continue;

                case H_ALINE:                           /* the Toolbox */
                        j->pc = PC68(ipc);
                        v = m68k_jit_h_aline(PC68(ipc));
                        budget -= j->native_n;
                        j->budget = budget;
                        if ((why = jump_slow(k, j, v, J_JUMP)))
                                return why;
                        budget = j->budget;
                        pc = j->pc - k->pbase;
                        continue;

                case H_MUL: case H_DIV: {
                        uint32_t *dn = &D(OP_RX);
                        if (!ea_ok(OP_EA, C_DATA))
                                goto musashi;
                        SRC(s, OP_EA, 2);
                        if (ht[op >> 6] == H_MUL) {
                                if (op & 0x100)
                                        v = (uint32_t)((int16_t)s * (int16_t)*dn);
                                else
                                        v = s * (*dn & 0xffff);
                                *dn = v;
                                flags_logic(j, v, 4);
                                continue;
                        }
                        if (s == 0) {
                                /* Divide by zero: the exception as Musashi takes
                                 * it (operand fetched, pc past it)
                                 */
                                j->pc = m68k_jit_h_div0(PC68(pc));
                                EXIT(IX_SYNC);
                        }
                        if (!(op & 0x100)) {            /* DIVU */
                                uint32_t q = *dn / s, r = *dn % s;
                                if (q < 0x10000) {
                                        *dn = (r << 16) | q;
                                        flags_logic(j, q, 2);
                                } else {
                                        j->v = 0x80;
                                }
                        } else {                        /* DIVS */
                                int32_t sd = (int16_t)s;
                                if (*dn == 0x80000000u && sd == -1) {
                                        *dn = 0;
                                        flags_logic(j, 0, 4);
                                        continue;
                                }
                                int32_t q = (int32_t)*dn / sd, r = (int32_t)*dn % sd;
                                if (q == (int16_t)q) {
                                        *dn = ((uint32_t)r << 16) | (q & 0xffff);
                                        flags_logic(j, q & 0xffff, 2);
                                } else {
                                        j->v = 0x80;
                                }
                        }
                        continue;
                }

                case H_OR_M: case H_AND_M: case H_SUB_M: case H_ADD_M: case H_EOR_M: {
                        /* Dn OP <ea> -> <ea>, and the register-pair forms */
                        int h = ht[op >> 6];
                        uint32_t *dn = &D(OP_RX);
                        sz = SZ_BYTES(OP_SZ);
                        ea = OP_EA;
                        if (h == H_EOR_M && ea < 8) {           /* EOR Dn,Dy */
                                uint32_t m = size_mask(sz), *dy = &D(ea);
                                v = (*dy ^ *dn) & m;
                                *dy = (*dy & ~m) | v;
                                flags_logic(j, v, sz);
                                continue;
                        }
                        if (ea < 0x10) {
                                if (h == H_EOR_M) {             /* CMPM (Ay)+,(Ax)+ */
                                        vp_t x = ea_read(k, pc, 0x18 | OP_RY, sz);
                                        vp_t y = ea_read(k, pc, 0x18 | OP_RX, sz);
                                        sub_f(j, VP_V(y), VP_V(x), sz, 0);
                                        continue;
                                }
                                if (h == H_SUB_M || h == H_ADD_M) {
                                        /* SUBX / ADDX */
                                        int op2 = h == H_SUB_M ? AR_SUBX : AR_ADDX;
                                        if (op & 0x8) {         /* -(Ay),-(Ax) */
                                                vp_t x = ea_read(k, pc, 0x20 | OP_RY, sz);
                                                vp_t y = ea_addr(k, pc, 0x20 | OP_RX, sz);
                                                v = arith(j, op2, rd(k, VP_V(y), sz), VP_V(x), sz);
                                                wr(k, VP_V(y), v, sz);
                                        } else {
                                                uint32_t m = size_mask(sz);
                                                v = arith(j, op2, *dn & m, D(OP_RY) & m, sz);
                                                *dn = (*dn & ~m) | v;
                                        }
                                        continue;
                                }
                                if (h == H_AND_M) {
                                        /* EXG (ABCD: Musashi) */
                                        uint32_t *rx, *ry;
                                        switch (op & 0x1f8) {
                                        case 0x140: rx = dn; ry = &D(OP_RY); break;
                                        case 0x148: rx = &A(OP_RX); ry = &A(OP_RY); break;
                                        case 0x188: rx = dn; ry = &A(OP_RY); break;
                                        default: goto musashi;
                                        }
                                        v = *rx;
                                        *rx = *ry;
                                        *ry = v;
                                        continue;
                                }
                                goto musashi;           /* SBCD */
                        }
                        if (ea > 0x39)
                                goto musashi;
                        s = *dn & size_mask(sz);
                        {
                                vp_t x = rmw_read(k, pc, ea, sz);
                                pc = VP_PC(x);
                                v = VP_V(x);
                        }
                        switch (h) {
                        case H_OR_M: v |= s; break;
                        case H_AND_M: v &= s; break;
                        case H_EOR_M: v ^= s; break;
                        case H_SUB_M: rmw_write(k, arith(j, AR_SUB, v, s, sz), sz); continue;
                        default: rmw_write(k, arith(j, AR_ADD, v, s, sz), sz); continue;
                        }
                        rmw_write(k, v, sz);
                        flags_logic(j, v, sz);
                        continue;
                }

                case H_BIT: {
                        /* BTST / BCHG / BCLR / BSET */
                        int type = OP_SZ;
                        ea = OP_EA;
                        if (op & 0x100) {
                                if ((ea & 0x38) == 0x08 || !ea_ok(ea, type ? C_DALT : C_DATA))
                                        goto musashi;           /* (incl. MOVEP) */
                                s = D(OP_RX);
                        } else {
                                if (!ea_ok(ea, type ? C_DALT : C_DATA & ~C_IMM))
                                        goto musashi;
                                s = FETCH16(pc);
                                pc += 2;
                        }
                        sz = ea < 8 ? 4 : 1;
                        s = 1u << (s & (8 * sz - 1));
                        if (!type) {
                                SRC(v, ea, sz);
                                j->not_z = v & s;
                                continue;
                        }
                        vp_t x = rmw_read(k, pc, ea, sz);
                        pc = VP_PC(x);
                        v = VP_V(x);
                        j->not_z = v & s;
                        v = type == 1 ? v ^ s : type == 2 ? v & ~s : v | s;
                        rmw_write(k, v, sz);
                        continue;
                }

                case H_IMM:
                        /* ORI ANDI SUBI ADDI EORI CMPI #imm,<ea> */
                        if ((op & 0x3f) == 0x3c) {
                                /* ... to SR (to CCR, and the invalid immediate
                                 * destinations: Musashi)
                                 */
                                uint32_t kind = op == 0x007c ? 0 : op == 0x027c ? 1 : op == 0x0a7c ? 2 : 3;
                                if (kind == 3)
                                        goto musashi;
                                v = FETCH16(pc);
                                j->pc = m68k_jit_h_srop(PC68(ipc), kind << 16 | v);
                                EXIT(IX_SYNC);
                        }
                        if (OP_SZ == 3 || (op & 0xe00) == 0xe00 || !ea_ok(OP_EA, C_DALT))
                                goto musashi;
                        sz = SZ_BYTES(OP_SZ);
                        if (sz == 4) {
                                s = FETCH32(pc);
                                pc += 4;
                        } else {
                                s = FETCH16(pc) & size_mask(sz);
                                pc += 2;
                        }
                        if ((op & 0xe00) == 0xc00) {            /* CMPI */
                                SRC(v, OP_EA, sz);
                                sub_f(j, v, s, sz, 0);
                                continue;
                        }
                        if (OP_EA < 8) {
                                uint32_t m = size_mask(sz), *dn = &D(OP_EA), d = *dn & m;
                                switch ((op >> 9) & 7) {
                                case 0: v = d | s; break;               /* ORI */
                                case 1: v = d & s; break;               /* ANDI */
                                case 5: v = d ^ s; break;               /* EORI */
                                default:                                /* SUBI / ADDI */
                                        v = op & 0x200 ? add_f(j, d, s, sz, 1) : sub_f(j, d, s, sz, 1);
                                        *dn = (*dn & ~m) | v;
                                        continue;
                                }
                                *dn = (*dn & ~m) | v;
                                flags_logic(j, v, sz);
                                continue;
                        }
                        {
                                vp_t x = rmw_read(k, pc, OP_EA, sz);
                                pc = VP_PC(x);
                                v = VP_V(x);
                        }
                        switch ((op >> 9) & 7) {
                        case 0: v |= s; break;                  /* ORI */
                        case 1: v &= s; break;                  /* ANDI */
                        case 5: v ^= s; break;                  /* EORI */
                        default:                                /* SUBI / ADDI */
                                rmw_write(k, arith(j, op & 0x200 ? AR_ADD : AR_SUB, v, s, sz), sz);
                                continue;
                        }
                        rmw_write(k, v, sz);
                        flags_logic(j, v, sz);
                        continue;

                case H_SHIFT_R: {
                        uint32_t cnt = OP_RX;
                        if (op & 0x20)
                                cnt = D(cnt) & 63;
                        else if (!cnt)
                                cnt = 8;
                        sz = SZ_BYTES(OP_SZ);
                        uint32_t *dy = &D(OP_RY), m = size_mask(sz);
                        v = shift(j, (op >> 3) & 3, op & 0x100, *dy, cnt, sz);
                        *dy = (*dy & ~m) | v;
                        continue;
                }

                case H_SHIFT_M: {
                        /* Memory shift by one (word) */
                        if (op & 0x800 || !ea_ok(OP_EA, C_MALT))
                                goto musashi;
                        vp_t x = rmw_read(k, pc, OP_EA, 2);
                        pc = VP_PC(x);
                        rmw_write(k, shift(j, (op >> 9) & 3, op & 0x100, VP_V(x), 1, 2), 2);
                        continue;
                }

                case H_MUSASHI:
                default:
                        goto musashi;
                }

        musashi:
                /* Anything else: Musashi, from the start of the instruction
                 * (the budget has been charged for it), then back to the
                 * dispatcher in case it changed modes or took an exception.
                 */
                j->pc = m68k_jit_h_fallback(PC68(ipc));
                EXIT(IX_SYNC);
        }
        j->pc = PC68(pc);
        budget++;                               /* (the loop test took one more) */
        EXIT(IX_BUDGET);
}
