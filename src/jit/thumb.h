/* Minimal Thumb-2 (ARMv7-M) instruction encoder for the 68k JIT.
 *
 * Mostly 32-bit encodings, so any of r0-r12/lr can be used anywhere.
 * Emitters write halfwords at e->p; on overflow they set e->full and stop.
 */
#ifndef THUMB_H
#define THUMB_H

#include <stdint.h>

typedef struct {
        uint16_t *p, *start, *end;
        int full;
        uint32_t fclob;         /* instructions emitted that may change APSR flags */
        int defer;              /* nonzero: "mrs r(defer - 1), APSR" is owed; it's
                                 * emitted before the next instruction that may
                                 * change the flags, and before any branch */
} temit_t;

enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC };
enum { C_EQ, C_NE, C_CS, C_CC, C_MI, C_PL, C_VS, C_VC, C_HI, C_LS, C_GE, C_LT, C_GT, C_LE, C_AL };
enum { SH_LSL, SH_LSR, SH_ASR, SH_ROR };

/* Data-processing opcodes (shared by immediate and register forms) */
enum { DP_AND = 0, DP_BIC = 1, DP_ORR = 2, DP_ORN = 3, DP_EOR = 4,
       DP_ADD = 8, DP_ADC = 10, DP_SBC = 11, DP_SUB = 13, DP_RSB = 14 };

/* Whether an instruction certainly leaves the APSR flags alone.  (Calls
 * count as clobbering; branches are emitted without being counted.)
 */
static inline int t_flagsafe16(uint16_t h)
{
        return (h >= 0x5000 && h < 0xA000) ||           /* ldr/str forms */
               (h & 0xFF00) == 0x4600 ||                /* mov (high regs) */
               (h & 0xFF00) == 0x4400 ||                /* add (high regs, T2: no S) */
               (h & 0xF500) == 0xB100 ||                /* cbz/cbnz */
               (h & 0xFF00) == 0xB200 ||                /* sxth/uxth/sxtb/uxtb */
               (h & 0xFF00) == 0xBA00 ||                /* rev/rev16/revsh */
               h == 0xBF00;                             /* nop */
}

static inline int t_flagsafe32(uint16_t h1, uint16_t h2)
{
        if ((h1 & 0xFE00) == 0xF800)                    /* ldr/str single */
                return 1;
        if ((h1 & 0xF800) == 0xF000 && !(h2 & 0x8000)) {
                if (h1 & 0x0200)                        /* plain binary immediate */
                        return 1;
                return !(h1 & 0x10);                    /* modified immediate, S = 0 */
        }
        if ((h1 & 0xFE00) == 0xEA00)                    /* shifted register, S = 0 */
                return !(h1 & 0x10);
        if ((h1 & 0xFF80) == 0xFA00) {
                if ((h2 & 0xF080) == 0xF080)            /* extends */
                        return 1;
                if ((h2 & 0xF0F0) == 0xF000)            /* register shifts, S = 0 */
                        return !(h1 & 0x10);
                return 0;
        }
        if ((h1 & 0xFFF0) == 0xFAB0 && (h2 & 0xF0F0) == 0xF080)    /* clz */
                return 1;
        return (h1 & 0xFF00) == 0xFB00;                 /* multiplies */
}

/* Emit an owed MRS now (it reads the flags, so doesn't count as a clobber) */
static inline void t_realize(temit_t *e)
{
        if (!e->defer)
                return;
        if (e->p + 2 > e->end) { e->full = 1; return; }
        e->p[0] = 0xF3EF;
        e->p[1] = 0x8000 | (e->defer - 1) << 8;
        e->p += 2;
        e->defer = 0;
}

static inline void t16(temit_t *e, uint16_t h)
{
        if (!t_flagsafe16(h)) {
                t_realize(e);
                e->fclob++;
        }
        if (e->p >= e->end) { e->full = 1; return; }
        *e->p++ = h;
}

static inline void t32(temit_t *e, uint16_t h1, uint16_t h2)
{
        if (!t_flagsafe32(h1, h2)) {
                t_realize(e);
                e->fclob++;
        }
        if (e->p + 2 > e->end) { e->full = 1; return; }
        e->p[0] = h1;
        e->p[1] = h2;
        e->p += 2;
}

/* ThumbExpandImm: returns the 12-bit i:imm3:imm8 encoding, or -1 */
static inline int t_modimm(uint32_t v)
{
        if (v < 256)
                return v;
        uint32_t b = v & 0xff, b1 = v >> 8 & 0xff;
        if (v == (b | b << 16))
                return 0x100 | b;
        if (v == (b1 << 8 | b1 << 24))
                return 0x200 | b1;
        if (v == b * 0x01010101u)
                return 0x300 | b;
        for (int rot = 8; rot < 32; rot++) {
                uint32_t x = (v << rot) | (v >> (32 - rot));    /* ROL undoes ROR */
                if (x < 256 && (x & 0x80))
                        return rot << 7 | (x & 0x7f);
        }
        return -1;
}

/* op Rd, Rn, #imm (modified immediate). Caller guarantees encodable. */
static inline void t_dpi(temit_t *e, int op, int s, int rd, int rn, uint32_t imm)
{
        int m = t_modimm(imm);
        t32(e, 0xF000 | (m >> 11 & 1) << 10 | op << 5 | s << 4 | rn,
            (m >> 8 & 7) << 12 | rd << 8 | (m & 0xff));
}

/* op Rd, Rn, Rm {, shift #n} */
static inline void t_dpr(temit_t *e, int op, int s, int rd, int rn, int rm, int sh, int n)
{
        n &= 31;
        t32(e, 0xEA00 | op << 5 | s << 4 | rn,
            (n >> 2) << 12 | rd << 8 | (n & 3) << 6 | sh << 4 | rm);
}

/* Common forms */
#define t_add(e, rd, rn, rm)            t_dpr(e, DP_ADD, 0, rd, rn, rm, SH_LSL, 0)
#define t_adds(e, rd, rn, rm)           t_dpr(e, DP_ADD, 1, rd, rn, rm, SH_LSL, 0)
#define t_sub(e, rd, rn, rm)            t_dpr(e, DP_SUB, 0, rd, rn, rm, SH_LSL, 0)
#define t_subs(e, rd, rn, rm)           t_dpr(e, DP_SUB, 1, rd, rn, rm, SH_LSL, 0)
#define t_ands(e, rd, rn, rm)           t_dpr(e, DP_AND, 1, rd, rn, rm, SH_LSL, 0)
#define t_orrs(e, rd, rn, rm)           t_dpr(e, DP_ORR, 1, rd, rn, rm, SH_LSL, 0)
#define t_eors(e, rd, rn, rm)           t_dpr(e, DP_EOR, 1, rd, rn, rm, SH_LSL, 0)
#define t_and(e, rd, rn, rm)            t_dpr(e, DP_AND, 0, rd, rn, rm, SH_LSL, 0)
#define t_orr(e, rd, rn, rm)            t_dpr(e, DP_ORR, 0, rd, rn, rm, SH_LSL, 0)
#define t_eor(e, rd, rn, rm)            t_dpr(e, DP_EOR, 0, rd, rn, rm, SH_LSL, 0)
#define t_cmp(e, rn, rm)                t_dpr(e, DP_SUB, 1, PC, rn, rm, SH_LSL, 0)
#define t_tst(e, rn, rm)                t_dpr(e, DP_AND, 1, PC, rn, rm, SH_LSL, 0)
/* mov{s} Rd, Rm, <shift> #n  (ORR with Rn = PC) */
#define t_movsh(e, s, rd, rm, sh, n)    t_dpr(e, DP_ORR, s, rd, PC, rm, sh, n)
/* mov Rd, Rm (16-bit T1, any registers, flags untouched) */
static inline void t_mov(temit_t *e, int rd, int rm)
{
        t16(e, 0x4600 | (rd & 8) << 4 | rm << 3 | (rd & 7));
}
#define t_mvn(e, rd, rm)                t_dpr(e, DP_ORN, 0, rd, PC, rm, SH_LSL, 0)
#define t_addi(e, rd, rn, imm)          t_dpi(e, DP_ADD, 0, rd, rn, imm)
#define t_subi(e, rd, rn, imm)          t_dpi(e, DP_SUB, 0, rd, rn, imm)
#define t_andi(e, rd, rn, imm)          t_dpi(e, DP_AND, 0, rd, rn, imm)
#define t_orri(e, rd, rn, imm)          t_dpi(e, DP_ORR, 0, rd, rn, imm)
#define t_eori(e, rd, rn, imm)          t_dpi(e, DP_EOR, 0, rd, rn, imm)
#define t_bici(e, rd, rn, imm)          t_dpi(e, DP_BIC, 0, rd, rn, imm)
#define t_cmpi(e, rn, imm)              t_dpi(e, DP_SUB, 1, PC, rn, imm)
#define t_tsti(e, rn, imm)              t_dpi(e, DP_AND, 1, PC, rn, imm)
#define t_rsbsi(e, rd, rn, imm)         t_dpi(e, DP_RSB, 1, rd, rn, imm)

/* Shift by register: op{s} Rd, Rn, Rm */
static inline void t_shr(temit_t *e, int sh, int s, int rd, int rn, int rm)
{
        t32(e, 0xFA00 | sh << 5 | s << 4 | rn, 0xF000 | rd << 8 | rm);
}

/* movs Rd, #imm8 (16-bit, low Rd, SETS FLAGS) */
static inline void t_movs8(temit_t *e, int rd, uint32_t imm8)
{
        t16(e, 0x2000 | rd << 8 | imm8);
}

static inline void t_movw(temit_t *e, int rd, uint32_t imm16)
{
        t32(e, 0xF240 | (imm16 >> 11 & 1) << 10 | (imm16 >> 12),
            (imm16 >> 8 & 7) << 12 | rd << 8 | (imm16 & 0xff));
}

static inline void t_movt(temit_t *e, int rd, uint32_t imm16)
{
        t32(e, 0xF2C0 | (imm16 >> 11 & 1) << 10 | (imm16 >> 12),
            (imm16 >> 8 & 7) << 12 | rd << 8 | (imm16 & 0xff));
}

/* Rd = any 32-bit constant (flags untouched) */
static inline void t_mov32(temit_t *e, int rd, uint32_t v)
{
        int m = t_modimm(v);
        if (m >= 0) {
                t_dpi(e, DP_ORR, 0, rd, PC, v);         /* mov.w */
                return;
        }
        m = t_modimm(~v);
        if (m >= 0) {
                t_dpi(e, DP_ORN, 0, rd, PC, ~v);        /* mvn.w */
                return;
        }
        t_movw(e, rd, v & 0xffff);
        if (v >> 16)
                t_movt(e, rd, v >> 16);
}

/* Loads/stores, positive 12-bit immediate offset */
enum { LS_STRB = 0xF880, LS_LDRB = 0xF890, LS_STRH = 0xF8A0, LS_LDRH = 0xF8B0,
       LS_STR = 0xF8C0, LS_LDR = 0xF8D0, LS_LDRSB = 0xF990, LS_LDRSH = 0xF9B0 };

static inline void t_ldst(temit_t *e, int op, int rt, int rn, uint32_t off)
{
        if (rt < 8 && rn < 8) {
                /* 16-bit T1 forms: imm5 scaled by the access size */
                switch (op) {
                case LS_LDR: case LS_STR:
                        if (!(off & 3) && off < 128) {
                                t16(e, (op == LS_LDR ? 0x6800 : 0x6000) | (off >> 2) << 6 | rn << 3 | rt);
                                return;
                        }
                        break;
                case LS_LDRH: case LS_STRH:
                        if (!(off & 1) && off < 64) {
                                t16(e, (op == LS_LDRH ? 0x8800 : 0x8000) | (off >> 1) << 6 | rn << 3 | rt);
                                return;
                        }
                        break;
                case LS_LDRB: case LS_STRB:
                        if (off < 32) {
                                t16(e, (op == LS_LDRB ? 0x7800 : 0x7000) | off << 6 | rn << 3 | rt);
                                return;
                        }
                        break;
                }
        }
        t32(e, op | rn, rt << 12 | (off & 0xfff));
}

/* Register-offset forms: [Rn, Rm] */
static inline void t_ldstr(temit_t *e, int op, int rt, int rn, int rm)
{
        if (rt < 8 && rn < 8 && rm < 8) {
                int o = -1;
                switch (op) {
                case LS_STR:   o = 0x5000; break;
                case LS_STRH:  o = 0x5200; break;
                case LS_STRB:  o = 0x5400; break;
                case LS_LDRSB: o = 0x5600; break;
                case LS_LDR:   o = 0x5800; break;
                case LS_LDRH:  o = 0x5A00; break;
                case LS_LDRB:  o = 0x5C00; break;
                case LS_LDRSH: o = 0x5E00; break;
                }
                if (o >= 0) {
                        t16(e, o | rm << 6 | rn << 3 | rt);
                        return;
                }
        }
        /* T2 encodings are the T3 opcode with bit 7 (the "imm12" flag) clear */
        t32(e, (op & ~0x80) | rn, rt << 12 | rm);
}

#define t_ldr(e, rt, rn, off)   t_ldst(e, LS_LDR, rt, rn, off)
#define t_str(e, rt, rn, off)   t_ldst(e, LS_STR, rt, rn, off)

static inline void t_rev(temit_t *e, int rd, int rm)
{
        if (rd < 8 && rm < 8)
                t16(e, 0xBA00 | rm << 3 | rd);
        else
                t32(e, 0xFA90 | rm, 0xF080 | rd << 8 | rm);
}

static inline void t_rev16(temit_t *e, int rd, int rm)
{
        if (rd < 8 && rm < 8)
                t16(e, 0xBA40 | rm << 3 | rd);
        else
                t32(e, 0xFA90 | rm, 0xF090 | rd << 8 | rm);
}
static inline void t_sxth(temit_t *e, int rd, int rm)  { t32(e, 0xFA0F, 0xF080 | rd << 8 | rm); }
static inline void t_uxth(temit_t *e, int rd, int rm)  { t32(e, 0xFA1F, 0xF080 | rd << 8 | rm); }
static inline void t_sxtb(temit_t *e, int rd, int rm)  { t32(e, 0xFA4F, 0xF080 | rd << 8 | rm); }
static inline void t_uxtb(temit_t *e, int rd, int rm)  { t32(e, 0xFA5F, 0xF080 | rd << 8 | rm); }
static inline void t_clz(temit_t *e, int rd, int rm)   { t32(e, 0xFAB0 | rm, 0xF080 | rd << 8 | rm); }
static inline void t_mul(temit_t *e, int rd, int rn, int rm) { t32(e, 0xFB00 | rn, 0xF000 | rd << 8 | rm); }

static inline void t_ubfx(temit_t *e, int rd, int rn, int lsb, int width)
{
        t32(e, 0xF3C0 | rn, (lsb >> 2) << 12 | rd << 8 | (lsb & 3) << 6 | (width - 1));
}

static inline void t_sbfx(temit_t *e, int rd, int rn, int lsb, int width)
{
        t32(e, 0xF340 | rn, (lsb >> 2) << 12 | rd << 8 | (lsb & 3) << 6 | (width - 1));
}

static inline void t_bfi(temit_t *e, int rd, int rn, int lsb, int width)
{
        int msb = lsb + width - 1;
        t32(e, 0xF360 | rn, (lsb >> 2) << 12 | rd << 8 | (lsb & 3) << 6 | msb);
}

static inline void t_mrs_apsr(temit_t *e, int rd)      { t32(e, 0xF3EF, 0x8000 | rd << 8); }
static inline void t_msr_apsr(temit_t *e, int rn)      { t32(e, 0xF380 | rn, 0x8800); }

static inline void t_push(temit_t *e, uint16_t list)   { t32(e, 0xE92D, list); }
static inline void t_pop(temit_t *e, uint16_t list)    { t32(e, 0xE8BD, list); }
static inline void t_blx(temit_t *e, int rm)           { t16(e, 0x4780 | rm << 3); }
static inline void t_bx(temit_t *e, int rm)            { t16(e, 0x4700 | rm << 3); }
static inline void t_nop(temit_t *e)                   { t16(e, 0xBF00); }

/* Branches.  Emit a placeholder and patch it once the target is known.
 *
 * Never read generated code back: on the Playdate a read pulls the line
 * into the (write-back) D-cache, and later writes to it, patches
 * included, then never reach the memory the CPU fetches instructions
 * from.  So the condition lives here, not in the placeholder.
 */
typedef struct {
        uint16_t *at;
        int cond;
        int narrow;             /* 16-bit form: B<c> T1 / B T2, forward up to 256 bytes */
} tbr_t;

static inline tbr_t t_b_placeholder(temit_t *e, int cond)
{
        t_realize(e);           /* (both paths get the flags) */
        tbr_t b = { e->p, cond, 0 };
        if (e->p + 2 > e->end) { e->full = 1; return b; }
        e->p[0] = e->p[1] = 0;  /* (branches don't touch the flags) */
        e->p += 2;
        return b;
}

/* A short forward branch over code that's known to be small */
static inline tbr_t t_b_placeholder_n(temit_t *e, int cond)
{
        t_realize(e);
        tbr_t b = { e->p, cond, 1 };
        if (e->p + 1 > e->end) { e->full = 1; return b; }
        e->p[0] = 0;
        e->p += 1;
        return b;
}

/* Can a narrow placeholder reach target? */
static inline int t_reaches_n(tbr_t b, uint16_t *target)
{
        int32_t off = (int32_t)((char *)target - ((char *)b.at + 4));
        return off >= 0 && off < (b.cond == C_AL ? 2048 : 256);
}

static inline void t_patch_branch(tbr_t b, uint16_t *target)
{
        uint16_t *at = b.at;
        int32_t off = (int32_t)((char *)target - ((char *)at + 4));
        uint32_t s = off < 0;
        if (b.narrow) {
                if (b.cond == C_AL)
                        at[0] = 0xE000 | (off >> 1 & 0x7ff);
                else
                        at[0] = 0xD000 | b.cond << 8 | (off >> 1 & 0xff);
                return;
        }
        if (b.cond == C_AL) {
                /* B.W T4: S:I1:I2:imm10:imm11, I1 = !(J1^S), I2 = !(J2^S) */
                uint32_t i1 = off >> 23 & 1, i2 = off >> 22 & 1;
                uint32_t j1 = !(i1 ^ s), j2 = !(i2 ^ s);
                at[0] = 0xF000 | s << 10 | (off >> 12 & 0x3ff);
                at[1] = 0x9000 | j1 << 13 | j2 << 11 | (off >> 1 & 0x7ff);
        } else {
                /* B<c>.W T3: S:J2:J1:imm6:imm11 */
                uint32_t j1 = off >> 18 & 1, j2 = off >> 19 & 1;
                at[0] = 0xF000 | s << 10 | b.cond << 6 | (off >> 12 & 0x3f);
                at[1] = 0x8000 | j1 << 13 | j2 << 11 | (off >> 1 & 0x7ff);
        }
}

/* BL to a known target (T1; same immediate layout as B.W T4) */
static inline void t_bl_to(temit_t *e, uint16_t *target)
{
        t_realize(e);           /* (first, so `at` is the BL) */
        uint16_t *at = e->p;
        t32(e, 0, 0);
        if (e->full)
                return;
        int32_t off = (int32_t)((char *)target - ((char *)at + 4));
        uint32_t s = off < 0;
        uint32_t i1 = off >> 23 & 1, i2 = off >> 22 & 1;
        uint32_t j1 = !(i1 ^ s), j2 = !(i2 ^ s);
        at[0] = 0xF000 | s << 10 | (off >> 12 & 0x3ff);
        at[1] = 0xD000 | j1 << 13 | j2 << 11 | (off >> 1 & 0x7ff);
}

/* Backwards/known-target branch */
static inline void t_b_to(temit_t *e, int cond, uint16_t *target)
{
        tbr_t b = t_b_placeholder(e, cond);
        if (!e->full)
                t_patch_branch(b, target);
}

#endif
