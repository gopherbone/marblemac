/* 68000 -> Thumb-2 JIT, layered on Musashi.  See m68kjit.h.
 *
 * Generated code runs from PSRAM through a 16KB I-cache, so it is built to
 * be small and straight-line: fast paths inline, slow paths ("stubs")
 * out of line at the end of each block, shared code in a permanent area.
 *
 * Blocks are entered through a trampoline that sets up:
 *   r4 = the register file (jregs_t), r5 = Mac RAM, r6 = code-page map,
 *   r7 = instruction budget (counts down; saved back on return)
 * and a block returns r0 = 68k instructions executed, with REG_PC set.
 *   r8, r10, r11 = scratch that survives helper calls
 *   r9 = ARM flags (APSR) captured from the last flag-setting instruction
 *   r0-r3, r12 = scratch (clobbered by helper calls)
 *
 * Flags are lazy: a flag-setting instruction just does MRS r9, APSR and
 * notes what kind of operation it was.  They're converted into Musashi's
 * format (N: bit 7, not_z: 0 = Z, V: bit 7, C/X: bit 8) by small shared
 * routines only at block exits, or before something reads them.
 */

#include <string.h>
#include "m68kcpu.h"
#include "m68kjit.h"
#include "m68knative.h"
#include "thumb.h"

/* umac memory accessors (slow paths) */
unsigned int cpu_read_byte(unsigned int address);
unsigned int cpu_read_word(unsigned int address);
unsigned int cpu_read_long(unsigned int address);
void cpu_write_byte(unsigned int address, unsigned int value);
void cpu_write_word(unsigned int address, unsigned int value);
void cpu_write_long(unsigned int address, unsigned int value);
extern int overlay;
#define PV_SONY_ADDR    0xc00069        /* uMac's paravirtual disk driver (machw.h) */

m68kjit_stats_t m68k_jit_stats;
int m68k_jit_enabled = 1;

#define JR_OFF(f)       ((uint32_t)offsetof(jregs_t, f))
#define OFF_D(n)        (JR_OFF(dar) + 4 * (n))
#define OFF_A(n)        (JR_OFF(dar) + 4 * (8 + (n)))
#define OFF_PC          JR_OFF(pc)
#define OFF_X           JR_OFF(x)
#define OFF_N           JR_OFF(n)
#define OFF_Z           JR_OFF(not_z)
#define OFF_V           JR_OFF(v)
#define OFF_C           JR_OFF(c)
#define OFF_BUDGET      JR_OFF(budget)
#define OFF_LASTSLOT    JR_OFF(lastexit)

#define PAGE_SHIFT      9               /* 512-byte code pages */
#define MAX_BLOCK_INSNS 48
#define BLOCK_SLACK     4096            /* halfwords kept free per instruction (incl. stubs) */
#define CODE_ALIGN      4               /* block alignment, bytes */
#define TABLE_BITS      13              /* sets; 2 ways each */

#define MAX_STUBS       96              /* out-of-line slow paths per block */
#define PERM_HALFWORDS  8192            /* trampoline + shared routines */

#define NOPAGE          0xffff
#define MAX_PAGES       4               /* code pages one block (trace) may span */
#define MAX_RANGES      12

typedef struct {
        uint32_t pc;
        uint32_t gen;                   /* global generation (bumped on recycle) */
        uint16_t *code;                 /* NULL: interpret one instruction */
        uint16_t pg[MAX_PAGES];         /* RAM pages the block's code came from */
        uint16_t gg[MAX_PAGES];         /* ... and their generations then */
} jit_entry_t;

static const m68kjit_platform_t *plat;
static uint8_t *ram;
static uint32_t ram_size;
static const uint8_t *rom;
static uint32_t rom_size;
/* Self-modifying code tracking.  codepage (checked inline by translated
 * stores) says a page holds translated code; codebits says exactly which
 * 16-bit words do, so data that merely shares a page with code doesn't
 * invalidate anything.  A hit bumps that page's generation, which retires
 * every block translated from it.
 */
static uint8_t *codepage;               /* one byte per RAM page */
static uint8_t *codebits;               /* one bit per 68k word of RAM */
static uint16_t *page_gen;
static uint32_t npages;
static jit_entry_t *table;
static uint32_t gen = 1;
static uint16_t *code_buf, *code_ptr, *code_end, *perm_end;
/* Testing: start generated code this many bytes later, to tell code
 * layout luck from real speedups (m68k_jit_code_skew can override it) */
#ifndef JIT_CODE_SKEW
#define JIT_CODE_SKEW 0
#endif
static uint16_t *entry_tramp;           /* uint32_t tramp(block | 1) */

/* Chaining.  Every chainable exit gets a record (its patchable slot and
 * the exit stub the slot starts out pointing at).  Linking an exit into a
 * RAM block also files the link under that block's page(s), so retiring
 * a page can point those exits back at their stubs.
 */
#define MAX_EXITS       32768
#define MAX_LINKS       32768
typedef struct { uint32_t slot, stub; } exit_rec_t;    /* byte offsets in code_buf */
/* stub's top bits: the exit left its flags unstored, so it may only be
 * linked to a block whose first instruction overwrites them (KF_*)
 */
#define EXIT_NEED_SHIFT 30
#define EXIT_STUB_MASK  ((1u << EXIT_NEED_SHIFT) - 1)
typedef struct { uint16_t exit_hi; uint16_t exit_lo; int32_t next; } link_t;
static exit_rec_t *exits;
static uint32_t nexits;
static link_t *links;
static int32_t nlinks;
static int32_t *page_links;             /* per page: first link, -1 = none */
static uint16_t *mat_fn[4][2];          /* materialise flags [kind][with X] */
static uint16_t *matx_fn[4];            /* ... just X [kind] */
static uint16_t *rd_fn[5], *wr_fn[5];   /* shared memory access, by size (1, 2, 4) */
static uint16_t *dyn_jump;              /* exit to a computed pc via the jump cache */
static uint16_t *exit_common;           /* chainable exit stubs: r0 = exit index + 1 */
static uint32_t *exit_pc;               /* per exit record: the 68k pc it leaves for */
static uint16_t *rts_fn;                /* RTS: pop pc, then on via dyn_jump */
/* Fused "address mode + access" entry points: [mode 0=(An) 1=(An)+ 2=-(An)]
 * [An][size 1,2,4].  rd: -> r0 = value.  wr: r1 = value.
 */
static uint16_t *ea_rd[3][8][5], *ea_wr[3][8][5];
/* Off: smaller code, but in the cache model the extra hot lines in the
 * shared area cost more I-cache misses than the bytes saved.
 */
int m68k_jit_fused_ea;
static uint16_t *helper_veneer[JH_COUNT];       /* bl-able stubs: mov32 r12, f; bx r12 */

/* Jump cache: 68k pc -> translated block (Thumb address of its body, past
 * the push), consulted by generated code at computed exits (RTS, JMP (An),
 * traps) so they don't have to go back to the dispatcher.  Wiped whenever
 * anything is invalidated, so it never points at stale code.
 */
#define JC_BITS         10
#define JC_EMPTY        0xffffffffu
#define JC_MISS         0xffffffffu     /* lastexit marker: dyn_jump missed */
typedef struct { uint32_t pc, code; } jc_t;
static jc_t *jcache;
int m68k_jit_no_jcache;                 /* testing: never fill it */
int m68k_jit_max_budget;                /* testing: cap instructions per run (0 = none) */
int m68k_jit_no_traces;                 /* testing: end blocks at conditional branches */
int m68k_jit_no_follow;                 /* testing: don't fold unconditional jumps into traces */
int m68k_jit_exit_flag_elide = 1;       /* chained exits skip storing flags the target overwrites */
int m68k_jit_defer_mrs = 1;             /* capture flags (MRS) only once something needs them */
int m68k_jit_inline_rts = 1;            /* exits to an RTS do it themselves */
/* Testing: perturb code layout (to tell real wins from cache-placement
 * luck).  skew: bytes left empty before the first block (multiple of 2;
 * build-time default JIT_CODE_SKEW); pad: bytes left empty after each block.
 */
int m68k_jit_code_skew = JIT_CODE_SKEW, m68k_jit_code_pad;
void (*m68k_jit_size_observer)(uint32_t pc, uint32_t op, uint32_t bytes);

static void jcache_clear(void)
{
        memset(jcache, 0xff, sizeof(jc_t) << JC_BITS);
}

static uint32_t jc_hash(uint32_t pc)
{
        return (pc * 0x9e3779b1u) >> (32 - JC_BITS);
}

/* -------------------------------------------------------------------- */
/* Invalidation                                                         */

void (*m68k_jit_flush_observer)(uint32_t addr);
void (*m68k_jit_translate_observer)(uint32_t pc, void *code, uint32_t len);

static uint32_t peek16(uint32_t a);

/* The active register file (on the dispatcher's stack while it runs) */
static jregs_t *J;

void m68k_jit_sync_out(void)
{
        memcpy(m68ki_cpu.dar, J->dar, sizeof J->dar);
        m68ki_cpu.pc = J->pc;
        m68ki_cpu.x_flag = J->x;
        m68ki_cpu.n_flag = J->n;
        m68ki_cpu.not_z_flag = J->not_z;
        m68ki_cpu.v_flag = J->v;
        m68ki_cpu.c_flag = J->c;
}

void m68k_jit_sync_in(void)
{
        memcpy(J->dar, m68ki_cpu.dar, sizeof J->dar);
        J->pc = m68ki_cpu.pc;
        J->x = m68ki_cpu.x_flag;
        J->n = m68ki_cpu.n_flag;
        J->not_z = m68ki_cpu.not_z_flag;
        J->v = m68ki_cpu.v_flag;
        J->c = m68ki_cpu.c_flag;
}

jregs_t *m68k_jit_regs(void)
{
        return J;
}

static void flush_all(void)
{
        gen++;
        memset(codepage, 0, npages);
        memset(codebits, 0, ram_size / 16);
        nexits = 0;
        nlinks = 0;
        jcache_clear();
        for (uint32_t p = 0; p < npages; p++)
                page_links[p] = -1;
}

static void unlink_page(uint32_t p)
{
        for (int32_t l = page_links[p]; l >= 0; l = links[l].next) {
                uint32_t e = (uint32_t)links[l].exit_hi << 16 | links[l].exit_lo;
                uint16_t *slot = (uint16_t *)((char *)code_buf + exits[e].slot);
                tbr_t b = { slot, C_AL };
                t_patch_branch(b, (uint16_t *)((char *)code_buf + (exits[e].stub & EXIT_STUB_MASK)));
                plat->code_written(slot, 4);
        }
        page_links[p] = -1;
}

static void invalidate_page(uint32_t p)
{
        unlink_page(p);
        jcache_clear();
        page_gen[p]++;
        codepage[p] = 0;
        memset(codebits + (p << PAGE_SHIFT) / 16, 0, (1u << PAGE_SHIFT) / 16);
        m68k_jit_stats.flushes++;
}

/* A RAM write of `size` bytes at offset addr: retire any code it hits */
static int check_write(uint32_t addr, uint32_t size)
{
        int hit = 0;
        if (!size || addr >= ram_size)
                return 0;
        uint32_t end = addr + size > ram_size ? ram_size : addr + size;
        for (uint32_t p = addr >> PAGE_SHIFT; p <= (end - 1) >> PAGE_SHIFT; p++) {
                if (!codepage[p])
                        continue;
                uint32_t lo = p << PAGE_SHIFT, hi = lo + (1u << PAGE_SHIFT);
                if (lo < addr)
                        lo = addr;
                if (hi > end)
                        hi = end;
                for (uint32_t w = lo >> 1; w <= (hi - 1) >> 1; w++) {
                        if (codebits[w >> 3] >> (w & 7) & 1) {
                                if (m68k_jit_flush_observer)
                                        m68k_jit_flush_observer(addr);
                                invalidate_page(p);
                                hit = 1;
                                break;
                        }
                }
        }
        return hit;
}

void (*m68k_jit_write_observer)(uint32_t addr, uint32_t size);

void m68k_jit_note_write(uint32_t addr, uint32_t size)
{
        if (m68k_jit_write_observer)
                m68k_jit_write_observer(addr, size);
        check_write(addr & 0xffffff, size);
}

static uint32_t h_rd8(uint32_t a, uint32_t b)  { (void)b; return cpu_read_byte(a); }
static uint32_t h_rd16(uint32_t a, uint32_t b) { (void)b; return cpu_read_word(a); }
static uint32_t h_rd32(uint32_t a, uint32_t b) { (void)b; return cpu_read_long(a); }
static uint32_t h_wr8(uint32_t a, uint32_t b)
{
        /* The paravirtual disk driver reads and writes 68k registers */
        int pv = (a & 0xffffff) == PV_SONY_ADDR;
        if (pv)
                m68k_jit_sync_out();
        cpu_write_byte(a, b);
        if (pv)
                m68k_jit_sync_in();
        return 0;
}
static uint32_t h_wr16(uint32_t a, uint32_t b) { cpu_write_word(a, b); return 0; }
static uint32_t h_wr32(uint32_t a, uint32_t b) { cpu_write_long(a, b); return 0; }
static uint32_t h_codewrite(uint32_t a, uint32_t b) { return check_write(a, b); }

/* A-line trap at pc (the Mac Toolbox): the 68000 pushes the PC of the
 * trap instruction and the SR, and jumps through the vector at $28.  Mac
 * OS runs in supervisor mode, so that's all there is to it; anything
 * unusual goes to Musashi.
 */
static uint32_t aline_dispatch(uint32_t sp, int alt, uint32_t *ninstr);
static uint32_t mv_read(uint32_t a, int lng);
static void mv_write(uint32_t a, uint32_t v, int lng);
#ifndef JIT_NATIVES
#define JIT_NATIVES     1
#endif
int m68k_jit_no_native = !JIT_NATIVES;  /* testing: run the ROM code instead */

/* Idle yield: an app calling GetNextEvent again within the same tick,
 * with nothing in the event queue, is just spinning until something
 * happens.  Stop the CPU there (at the trap's routine, as if it were a
 * slow CPU) until the next interrupt, like STOP.  The block drains its
 * budget (IDLE_DRAIN, taken back off afterwards) to get out promptly.
 */
#ifndef JIT_IDLE_YIELD
#define JIT_IDLE_YIELD  1
#endif
int m68k_jit_idle_yield = JIT_IDLE_YIELD;
#define IDLE_DRAIN      M68K_JIT_IDLE_DRAIN
int m68k_jit_idle_request;
static int sleeping;
static uint32_t gne_prev_ticks;

static void check_idle(uint32_t trap)
{
        if (trap != 0xa970 || !m68k_jit_idle_yield || m68ki_cpu.int_mask)
                return;                         /* GetNextEvent, interrupts on */
        uint32_t ticks = mv_read(0x16a, 1), qhead = mv_read(0x14c, 1);
        if (qhead == 0 && ticks == gne_prev_ticks) {
                m68k_jit_idle_request = 1;
                gne_prev_ticks = ticks + 1;     /* the pass after waking counts for that tick */
        } else {
                gne_prev_ticks = ticks;
        }
}

static uint32_t h_aline_body(uint32_t pc);

static uint32_t h_aline(uint32_t pc, uint32_t b)
{
        (void)b;
        float t0 = plat->now ? plat->now() : 0;
        uint32_t r = h_aline_body(pc);
        if (plat->now)
                m68k_jit_stats.t_aline += plat->now() - t0;
        return r;
}

static uint32_t h_aline_body(uint32_t pc)
{
        J->native_n = 0;
        check_idle(peek16(pc));
        if (m68k_jit_idle_request)
                J->native_n = IDLE_DRAIN;
        if (m68ki_cpu.s_flag != SFLAG_SET || m68ki_cpu.t1_flag || m68ki_cpu.t0_flag) {
                m68k_jit_sync_out();
                REG_PPC = pc;
                REG_PC = pc + 2;
                REG_IR = peek16(pc);
                m68ki_exception_1010();
                m68k_jit_sync_in();
                return J->pc;
        }
        uint32_t ccr = ((J->x & 0x100) ? 0x10 : 0) | ((J->n & 0x80) ? 8 : 0) |
                       (J->not_z ? 0 : 4) | ((J->v & 0x80) ? 2 : 0) | ((J->c & 0x100) ? 1 : 0);
        uint32_t sr = (m68ki_cpu.s_flag << 11) | (m68ki_cpu.m_flag << 11) | m68ki_cpu.int_mask | ccr;
        uint32_t a7 = J->dar[15] - 4;
        cpu_write_long(a7 & 0xffffff, pc);
        a7 -= 2;
        cpu_write_word(a7 & 0xffffff, sr);
        J->dar[15] = a7;
        uint32_t vec = cpu_read_long((m68ki_cpu.vbr + 0x28) & 0xffffff);
        if ((vec == 0x401f52 || vec == 0x401f4a) && !m68k_jit_no_native) {
                uint32_t n;
                uint32_t to = aline_dispatch(a7, vec == 0x401f4a, &n);
                J->native_n += n;
                m68k_jit_stats.native_calls++;
                m68k_jit_stats.native_instrs += n;
                return to;
        }
        return vec;
}

/* The ROM's trap dispatcher (Plus v3), entered with the A-line exception
 * frame (SR, then the trap's own address) at sp; does what its 68k code
 * does up to the jump into the trap's routine, and says how many
 * instructions that was.  The A-line vector points at 401f52; 401f4a
 * (alt) is the other way in.
 *
 *  401f4a  move.l  ($2,A7), ($4,A7)        401f88  lea     $400.w, A2
 *  401f50  bra     $401f54                 401f8c  bclr    #$8, D2
 *  401f52  subq.l  #2, A7
 *  401f54  movem.l D1-D2/A2, -(A7)         401f90  bne     $401fac
 *  401f58  movea.l ($10,A7), A2            401f92  lsl.w   #2, D2
 *  401f5c  move.w  (A2)+, D2               401f94  movea.l (A2,D2.w), A2
 *  401f5e  move.l  A2, ($10,A7)            401f98  movem.l A0-A1, -(A7)
 *  401f62  move.w  D2, D1                  401f9c  jsr     (A2)
 *  401f64  andi.w  #$1ff, D2               ...
 *  401f68  cmpi.w  #-$5800, D1             401fac  lsl.w   #2, D2
 *  401f6c  bcs     $401f88                 401fae  movea.l (A2,D2.w), A2
 *  401f6e  lea     $c00.w, A2              401fb2  move.l  A1, -(A7)
 *  401f72  lsl.w   #2, D2                  401fb4  jsr     (A2)
 *  401f74  move.l  (A2,D2.w), ($c,A7)
 *  401f7a  cmpi.w  #-$5400, D1
 *  401f7e  movem.l (A7)+, D1-D2/A2
 *  401f82  bcs     $401f86
 *  401f84  move.l  (A7)+, (A7)
 *  401f86  rts
 */
static uint32_t aline_dispatch(uint32_t sp, int alt, uint32_t *ninstr)
{
#define RD16(a)         mv_read((a) & 0xffffff, 0)
#define RD32(a)         mv_read((a) & 0xffffff, 1)
#define WR32(a, v)      mv_write((a) & 0xffffff, (v), 1)
        uint32_t d1 = J->dar[1], d2 = J->dar[2], a2 = J->dar[10];
        uint32_t n = alt ? 2 : 1;               /* move.l + bra, or subq */
        if (alt)
                WR32(sp + 4, RD32(sp + 2));
        else
                sp -= 2;
        sp -= 12;
        WR32(sp, d1);
        WR32(sp + 4, d2);
        WR32(sp + 8, a2);
        a2 = RD32(sp + 16);
        uint32_t trap = RD16(a2);
        a2 += 2;
        WR32(sp + 16, a2);
        uint32_t idx = trap & 0x1ff;
        if (trap >= 0xa800) {
                /* Toolbox: the routine's address replaces the frame's SR
                 * word, and the RTS goes there (one return address
                 * further up for auto-pop traps).
                 */
                idx = (idx << 2) & 0xffff;              /* lsl.w #2: X = bit 14 = 0 */
                J->x = 0;
                WR32(sp + 12, RD32(0xc00 + (uint32_t)(int16_t)idx));
                uint32_t res = trap - 0xac00;           /* cmpi.w #$ac00, D1 */
                J->n = (res >> 8) & 0xff;
                J->not_z = res & 0xffff;
                J->v = ((0xac00 ^ trap) & (res ^ trap)) >> 8 & 0x80;
                J->c = (res >> 8) & 0x100;
                sp += 12;                               /* movem restores D1/D2/A2 */
                n += 15;
                if (trap >= 0xac00) {
                        uint32_t v = RD32(sp);          /* move.l (A7)+, (A7) */
                        sp += 4;
                        WR32(sp, v);
                        J->n = v >> 24;
                        J->not_z = v;
                        J->v = J->c = 0;
                        n++;
                }
                uint32_t to = RD32(sp);
                J->dar[15] = sp + 4;
                *ninstr = n;
                return to;
        }
        /* OS: D1 = the trap word, D2 = its table offset, and the routine
         * returns into the dispatcher, which restores registers.
         */
        int keep_a0 = idx & 0x100;                      /* bclr #8; bne */
        idx &= 0xff;
        uint32_t off = idx << 2;                        /* lsl.w #2: C/X = bit 14 = 0 */
        J->x = J->c = 0;
        J->v = 0;
        J->n = (off >> 8) & 0x80;
        J->not_z = off;
        a2 = RD32(0x400 + off);
        uint32_t ret;
        if (!keep_a0) {
                sp -= 8;
                WR32(sp, J->dar[8]);
                WR32(sp + 4, J->dar[9]);
                ret = 0x401f9e;
        } else {
                sp -= 4;
                WR32(sp, J->dar[9]);                    /* move.l A1, -(A7) */
                J->n = J->dar[9] >> 24;
                J->not_z = J->dar[9];
                J->c = 0;
                ret = 0x401fb6;
        }
        sp -= 4;
        WR32(sp, ret);
        J->dar[1] = (d1 & 0xffff0000u) | trap;
        J->dar[2] = (d2 & 0xffff0000u) | off;
        J->dar[10] = a2;
        J->dar[15] = sp;
        *ninstr = n + 15;
        return a2;
#undef RD16
#undef RD32
#undef WR32
}

uint32_t m68k_jit_helper(int n, uint32_t a, uint32_t b)
{
        switch (n) {
        case JH_RD8:    return cpu_read_byte(a);
        case JH_RD16:   return cpu_read_word(a);
        case JH_RD32:   return cpu_read_long(a);
        case JH_WR8:    return h_wr8(a, b);
        case JH_WR16:   cpu_write_word(a, b); return 0;
        case JH_WR32:   cpu_write_long(a, b); return 0;
        case JH_CODEWRITE:
                return check_write(a, b);
        case JH_ALINE:
                return h_aline(a, b);
        default:
                return m68k_jit_helper_fn[n](a, b);
        }
        return 0;
}

/* Status register, from the JIT's flags and Musashi's mode bits */
static uint32_t get_sr_j(void)
{
        uint32_t ccr = ((J->x & 0x100) ? 0x10 : 0) | ((J->n & 0x80) ? 8 : 0) |
                       (J->not_z ? 0 : 4) | ((J->v & 0x80) ? 2 : 0) | ((J->c & 0x100) ? 1 : 0);
        return m68ki_cpu.t1_flag | m68ki_cpu.t0_flag | (m68ki_cpu.s_flag << 11) |
               (m68ki_cpu.m_flag << 11) | m68ki_cpu.int_mask | ccr;
}

/* For native routines: the SR the 68k would see now */
uint32_t m68k_jit_get_sr(void)
{
        return get_sr_j();
}

/* One instruction at pc through Musashi */
static uint32_t h_interp(uint32_t pc, uint32_t b)
{
        (void)b;
        J->pc = pc;
        m68k_jit_sync_out();
        m68k_step_one();
        m68k_jit_sync_in();
        return J->pc;
}

/* SR = v, then carry on at next.  Staying in supervisor mode without
 * tracing (the Mac's normal state) only changes the CCR and interrupt
 * mask; the dispatcher checks for interrupts when the block ends.
 * Anything else goes through Musashi (stack switch, interrupt check).
 */
static uint32_t set_sr_then(uint32_t v, uint32_t next)
{
        v &= m68ki_cpu.sr_mask;
        /* (An interrupt this unmasks is taken right away, as Musashi does.) */
        if (m68ki_cpu.s_flag == SFLAG_SET && (v & 0x2000) && !(v & 0xc000) &&
            !m68ki_cpu.nmi_pending && CPU_INT_LEVEL <= (v & 0x0700)) {
                J->x = (v & 0x10) << 4;
                J->n = (v & 8) << 4;
                J->not_z = !(v & 4);
                J->v = (v & 2) << 6;
                J->c = (v & 1) << 8;
                m68ki_cpu.int_mask = v & 0x0700;
                return next;
        }
        J->pc = next;
        m68k_jit_sync_out();
        m68ki_set_sr(v);
        m68k_jit_sync_in();
        return J->pc;
}

static uint32_t h_getsr(uint32_t a, uint32_t b)
{
        (void)a; (void)b;
        return get_sr_j();
}

static uint32_t h_srop(uint32_t pc, uint32_t arg)
{
        if (m68ki_cpu.s_flag != SFLAG_SET)
                return h_interp(pc, 0);         /* privilege violation */
        uint32_t imm = arg & 0xffff, sr = get_sr_j();
        switch (arg >> 16) {
        case 0: sr |= imm; break;
        case 1: sr &= imm; break;
        default: sr ^= imm; break;
        }
        return set_sr_then(sr, pc + 4);
}

/* arg: new SR in the low half, extra instruction bytes (an immediate) in the high */
static uint32_t h_movetosr(uint32_t pc, uint32_t arg)
{
        if (m68ki_cpu.s_flag != SFLAG_SET)
                return h_interp(pc, 0);
        return set_sr_then(arg & 0xffff, pc + 2 + (arg >> 16));
}

static uint32_t h_rte(uint32_t pc, uint32_t b)
{
        (void)b;
        if (m68ki_cpu.s_flag != SFLAG_SET)
                return h_interp(pc, 0);
        uint32_t a7 = J->dar[15];
        uint32_t sr = cpu_read_word(a7 & 0xffffff);
        uint32_t npc = cpu_read_long((a7 + 2) & 0xffffff);
        J->dar[15] = a7 + 6;
        return set_sr_then(sr, npc);
}

/* MOVEM, in one call instead of an unrolled load/store per register.
 * spec: bits 0-15 register mask, 16 = long, 17 = memory to registers,
 * 18 = -(An) (store) / (An)+ (load) addressing, 20-22 = An.
 */
#define MV_LONG         (1u << 16)
#define MV_TOREGS       (1u << 17)
#define MV_AUTO         (1u << 18)

static uint32_t mv_read(uint32_t a, int lng)
{
        a &= 0xffffff;
        if (!(a & 0xc00000)) {
                const uint8_t *p = ram + a;
                return lng ? (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3] : (uint32_t)p[0] << 8 | p[1];
        }
        return lng ? cpu_read_long(a) : cpu_read_word(a);
}

static void mv_write(uint32_t a, uint32_t v, int lng)
{
        a &= 0xffffff;
        if (!(a & 0xc00000)) {
                m68k_jit_note_write(a, lng ? 4 : 2);
                uint8_t *p = ram + a;
                if (lng) {
                        p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
                } else {
                        p[0] = v >> 8; p[1] = v;
                }
                return;
        }
        if (lng)
                cpu_write_long(a, v);
        else
                cpu_write_word(a, v & 0xffff);
}

/* Memory access for native routines (m68knative.c) */
uint32_t m68k_jit_read(uint32_t addr, int size)
{
        if (size == 1) {
                addr &= 0xffffff;
                return addr & 0xc00000 ? cpu_read_byte(addr) : ram[addr];
        }
        return mv_read(addr, size == 4);
}

void m68k_jit_write(uint32_t addr, uint32_t v, int size)
{
        if (size == 1) {
                addr &= 0xffffff;
                if (addr & 0xc00000) {
                        cpu_write_byte(addr, v & 0xff);
                } else {
                        m68k_jit_note_write(addr, 1);
                        ram[addr] = v;
                }
                return;
        }
        mv_write(addr, v, size == 4);
}

static uint32_t h_native(uint32_t idx, uint32_t pc)
{
        uint32_t n = 0;
        float t0 = plat->now ? plat->now() : 0;
        uint32_t next = m68k_natives[idx].fn(J, pc, &n);
        if (plat->now)
                m68k_jit_stats.t_native += plat->now() - t0;
        J->native_n = n;
        m68k_native_stats[idx].calls++;
        m68k_native_stats[idx].instrs += n;
        m68k_jit_stats.native_calls++;
        m68k_jit_stats.native_instrs += n;
        return next;
}

static uint32_t h_movem(uint32_t ea, uint32_t spec)
{
        int lng = !!(spec & MV_LONG), size = lng ? 4 : 2, an = 8 + (spec >> 20 & 7);
        uint32_t mask = spec & 0xffff;
        uint32_t bytes = (uint32_t)__builtin_popcount(mask) * size;
        /* Fast path: the whole transfer is in RAM.  One write check covers
         * it, and only the registers in the mask are visited.
         */
        uint32_t lo = (spec & MV_TOREGS) || !(spec & MV_AUTO) ? ea : ea - bytes;
        lo &= 0xffffff;
        if (bytes && lo + bytes <= ram_size) {
                uint8_t *p = ram + lo;
                if (spec & MV_TOREGS) {
                        while (mask) {
                                int i = __builtin_ctz(mask);
                                mask &= mask - 1;
                                if (lng) {
                                        uint32_t v;
                                        memcpy(&v, p, 4);
                                        J->dar[i] = __builtin_bswap32(v);
                                } else {
                                        J->dar[i] = (uint32_t)(int16_t)(p[0] << 8 | p[1]);
                                }
                                p += size;
                        }
                        if (spec & MV_AUTO)
                                J->dar[an] = ea + bytes;
                        return 0;
                }
                m68k_jit_note_write(lo, bytes);
                /* -(An) takes the mask reversed (bit i = register 15 - i);
                 * either way the lowest address gets the lowest register.
                 */
                int rev = !!(spec & MV_AUTO);
                while (mask) {
                        int i;
                        if (rev) {
                                i = 31 - __builtin_clz(mask);
                                mask &= ~(1u << i);
                                i = 15 - i;
                        } else {
                                i = __builtin_ctz(mask);
                                mask &= mask - 1;
                        }
                        uint32_t v = J->dar[i];
                        if (lng) {
                                v = __builtin_bswap32(v);
                                memcpy(p, &v, 4);
                        } else {
                                p[0] = v >> 8;
                                p[1] = v;
                        }
                        p += size;
                }
                if (spec & MV_AUTO)
                        J->dar[an] = ea - bytes;
                return 0;
        }
        if (spec & MV_TOREGS) {
                for (int i = 0; i < 16; i++) {
                        if (!(mask & (1u << i)))
                                continue;
                        uint32_t v = mv_read(ea, lng);
                        J->dar[i] = lng ? v : (uint32_t)(int16_t)v;
                        ea += size;
                }
                if (spec & MV_AUTO)
                        J->dar[an] = ea;
        } else if (spec & MV_AUTO) {
                for (int i = 0; i < 16; i++) {
                        if (!(mask & (1u << i)))
                                continue;
                        ea -= size;
                        mv_write(ea, J->dar[15 - i], lng);
                }
                J->dar[an] = ea;
        } else {
                for (int i = 0; i < 16; i++) {
                        if (!(mask & (1u << i)))
                                continue;
                        mv_write(ea, J->dar[i], lng);
                        ea += size;
                }
        }
        return 0;
}

uint32_t (*const m68k_jit_helper_fn[JH_COUNT])(uint32_t, uint32_t) = {
        h_rd8, h_rd16, h_rd32, h_wr8, h_wr16, h_wr32, h_codewrite, h_aline,
        h_getsr, h_srop, h_movetosr, h_rte, h_interp, h_movem, h_native,
};

/* -------------------------------------------------------------------- */
/* Translation context                                                  */

enum { ST_EXIT, ST_SMC, ST_BRANCH };

typedef struct {
        tbr_t from;             /* branch into the stub */
        tbr_t from2;            /* ST_EXIT: the chain slot, also into the stub */
        uint16_t *ret;          /* where it continues */
        uint32_t pc;            /* ST_EXIT/ST_SMC: where the 68k goes next */
        uint8_t type, size, ra, rv;
        uint8_t ninstr;         /* ST_SMC */
        int8_t fpend, fxpend, fr9kind;  /* ST_SMC: flag state at the store; ST_EXIT: unstored flags */
        uint8_t need;           /* ST_EXIT: flags left unstored (KF_* the target must kill) */
        uint8_t fdefer;         /* ST_EXIT: MRS still owed (temit_t.defer) */
} stub_t;

enum { F_NONE = -1, F_ADD, F_SUB, F_LOGIC, F_SHIFT };

/* Lazy flag state during translation */
typedef struct {
        int pend;               /* r9 holds NZVC not yet stored (kind), or F_NONE */
        int xpend;              /* ... and X should be taken from r9 too */
        int r9kind;             /* r9 still matches the stored flags (kind) */
        uint32_t apsr;          /* e.fclob + 1 while APSR still equals r9, else 0 */
} fstate_t;

typedef struct {
        temit_t e;
        uint32_t start_pc;
        uint32_t pc;            /* address of the instruction being translated */
        uint32_t fetch;         /* next extension word */
        int count;              /* instructions completed before this one */
        int fail;               /* couldn't fetch / unsupported */
        fstate_t f;
        int alu_imm_valid;      /* emit_alu: source is alu_imm, not r8 */
        uint32_t follow;        /* translate_one returned 2: carry on at this pc */
        uint32_t rstart[MAX_RANGES], rend[MAX_RANGES];  /* 68k code the trace covers */
        int nranges;
        int closed;             /* rend[nranges - 1] is the last range (not open) */
        uint32_t xw[4];         /* single 68k words the code also depends on */
        int nxw;
        uint32_t alu_imm;
        stub_t stubs[MAX_STUBS];
        int nstubs;
} tctx_t;

static int fetchable(uint32_t a)
{
        a &= 0xffffff;
        return a + 2 <= ram_size || ((a & 0xf00000) == 0x400000);
}

static uint32_t peek16(uint32_t a)
{
        a &= 0xffffff;
        if (a + 2 <= ram_size)
                return ram[a] << 8 | ram[a + 1];
        if ((a & 0xf00000) == 0x400000) {
                a &= rom_size - 1;
                return rom[a] << 8 | rom[a + 1];
        }
        return 0;
}

static uint32_t fetch16(tctx_t *t)
{
        if (!fetchable(t->fetch)) {
                t->fail = 1;
                return 0;
        }
        uint32_t v = peek16(t->fetch);
        t->fetch += 2;
        return v;
}

static uint32_t fetch32(tctx_t *t)
{
        uint32_t hi = fetch16(t);
        return hi << 16 | fetch16(t);
}

#define E       (&t->e)

static void call_helper(tctx_t *t, int n)
{
        t_bl_to(E, helper_veneer[n]);
}

/* rd = rn + constant (any value) */
static void add_const(tctx_t *t, int rd, int rn, int32_t v)
{
        if (v == 0) {
                if (rd != rn)
                        t_mov(E, rd, rn);
        } else if (v > 0 && t_modimm(v) >= 0) {
                t_addi(E, rd, rn, v);
        } else if (v < 0 && t_modimm(-v) >= 0) {
                t_subi(E, rd, rn, -v);
        } else {
                t_mov32(E, R12, (uint32_t)v);
                t_add(E, rd, rn, R12);
        }
}

/* -------------------------------------------------------------------- */
/* Memory access                                                        */


/* r0 = zero-extended value of `size` bytes at address in ra.  Calls a
 * shared routine (which stays in the I-cache) rather than inlining the
 * RAM/ROM/IO decode at every access.  Clobbers r0-r3, r12, lr.
 */
static void emit_load(tctx_t *t, int size, int ra)
{
        if (ra != R0)
                t_mov(E, R0, ra);
        t_bl_to(E, rd_fn[size]);
}

static void emit_store_ck(tctx_t *t, int size, int ra, int rv, int check);
static void emit_smc_check(tctx_t *t);
static void emit_flush_flags(tctx_t *t);
static void emit_exit_const(tctx_t *t, uint32_t pc, int ninstr);
static int can_follow(tctx_t *t, uint32_t target);
static int add_word_dep(tctx_t *t, uint32_t pc);

/* Store `size` bytes of rv at address ra, as the last thing the current
 * instruction does.  If the store overwrote translated code, leave the
 * block right after this instruction: what follows may be what changed.
 * Clobbers r0-r3, r12, lr.
 */
static void emit_store(tctx_t *t, int size, int ra, int rv)
{
        emit_store_ck(t, size, ra, rv, 1);
}

/* ... for stores that aren't the instruction's last action */
static void emit_store_nochk(tctx_t *t, int size, int ra, int rv)
{
        emit_store_ck(t, size, ra, rv, 0);
}

static void emit_store_ck(tctx_t *t, int size, int ra, int rv, int check)
{
        if (rv == R0) {
                t_mov(E, R1, R0);
                if (ra != R0)
                        t_mov(E, R0, ra);
        } else {
                if (ra != R0)
                        t_mov(E, R0, ra);
                if (rv != R1)
                        t_mov(E, R1, rv);
        }
        t_bl_to(E, wr_fn[size]);
        if (check)
                emit_smc_check(t);
}

/* After a store that was the instruction's last action: r0 != 0 means it
 * overwrote translated code, so leave the block after this instruction.
 */
static void emit_smc_check(tctx_t *t)
{
        if (t->nstubs >= MAX_STUBS) {
                t->fail = 1;
                return;
        }
        t16(E, 0xB108);                         /* cbz r0, past the b.w (target = here + 4 + 2) */
        stub_t *st = &t->stubs[t->nstubs++];
        st->type = ST_SMC;
        st->pc = t->fetch;
        st->ninstr = t->count + 1;
        st->fpend = t->f.pend;
        st->fxpend = t->f.xpend;
        st->fr9kind = t->f.r9kind;
        st->from = t_b_placeholder(E, C_AL);
        st->ret = NULL;
}

static void emit_stub(tctx_t *t, stub_t *st)
{
        if (st->from.at)
                t_patch_branch(st->from, E->p);
        t->f.apsr = 0;
        E->defer = 0;   /* (every path into a stub has captured the flags) */
        switch (st->type) {
        case ST_SMC:
        case ST_BRANCH: {
                fstate_t now = t->f;
                t->f.pend = st->fpend;
                t->f.xpend = st->fxpend;
                t->f.r9kind = st->fr9kind;
                if (st->type == ST_SMC)
                        m68k_jit_stats.smc_exits++;
                emit_exit_const(t, st->pc, st->ninstr);
                t->f = now;
                return;
        }
        case ST_EXIT: {
                uint32_t idx = nexits < MAX_EXITS ? nexits++ : MAX_EXITS;
                if (idx < MAX_EXITS) {
                        exits[idx].slot = (uint32_t)((char *)st->from2.at - (char *)code_buf);
                        exits[idx].stub = (uint32_t)((char *)E->p - (char *)code_buf) |
                                          (uint32_t)st->need << EXIT_NEED_SHIFT;
                }
                t_patch_branch(st->from2, E->p);
                if (st->need) {
                        /* The inline exit skipped storing the flags (the
                         * next block overwrites them); leaving for the
                         * dispatcher, they have to be right.
                         */
                        E->defer = st->fdefer;
                        fstate_t now = t->f;
                        t->f.pend = st->fpend;
                        t->f.xpend = st->fxpend;
                        t->f.r9kind = st->fr9kind;
                        emit_flush_flags(t);
                        t->f = now;
                }
                if (idx < MAX_EXITS) {
                        /* exit_common stores lastexit and the pc */
                        exit_pc[idx] = st->pc;
                        t_mov32(E, R0, idx + 1);
                        t_b_to(E, C_AL, exit_common);
                        return;
                }
                t_mov32(E, R0, st->pc);
                t_str(E, R0, R4, OFF_PC);
                t16(E, 0xBD08);                 /* pop {r3, pc} */
                return;
        }
        }
        t_b_to(E, C_AL, st->ret);
}

/* -------------------------------------------------------------------- */
/* Flags                                                                */

/* Store X from r9 (as computed by an op of `kind`) */
static void emit_store_x(tctx_t *t, int kind)
{
        t_realize(E);
        if (kind != F_LOGIC && kind != F_NONE) {
                t_bl_to(E, matx_fn[kind]);      /* (4 bytes instead of 10-14) */
                return;
        }
        t_movsh(E, 0, R3, R9, SH_LSR, 21);
        t_andi(E, R3, R3, 0x100);
        if (kind == F_SUB)
                t_eori(E, R3, R3, 0x100);
        t_str(E, R3, R4, OFF_X);
}

/* Make Musashi's flag fields current */
static void emit_flush_flags(tctx_t *t)
{
        if (t->f.pend != F_NONE) {
                t_bl_to(E, mat_fn[t->f.pend][t->f.xpend]);
                t->f.r9kind = t->f.pend;
                t->f.pend = F_NONE;
                t->f.xpend = 0;
        } else if (t->f.xpend) {
                emit_store_x(t, t->f.r9kind);
                t->f.xpend = 0;
        }
}

/* About to overwrite r9 with flags from a new op */
static void retire_r9(tctx_t *t, int new_sets_x)
{
        if (t->f.xpend && !new_sets_x) {
                emit_store_x(t, t->f.pend != F_NONE ? t->f.pend : t->f.r9kind);
                t->f.xpend = 0;
        }
}

/* The op just emitted set ARM NZCV; capture them (lazily) */
static void emit_flags(tctx_t *t, int kind, int set_x)
{
        retire_r9(t, set_x);
        if (m68k_jit_defer_mrs)
                E->defer = R9 + 1;      /* emitted when something needs r9 or changes APSR */
        else
                t_mrs_apsr(E, R9);
        t->f.apsr = E->fclob + 1;
        t->f.pend = kind;
        t->f.xpend = set_x;
        t->f.r9kind = F_NONE;
}

/* Flags known at translation time (X untouched): a synthetic APSR */
static void emit_flags_const(tctx_t *t, int n, int z, int v, int c)
{
        retire_r9(t, 0);
        E->defer = 0;                   /* r9 gets overwritten / the flags are dead */
        if (v || c) {
                /* Not expressible as LOGIC; store directly */
                t_mov32(E, R2, n ? 0x80 : 0);
                t_str(E, R2, R4, OFF_N);
                t_mov32(E, R2, z ? 0 : 1);
                t_str(E, R2, R4, OFF_Z);
                t_mov32(E, R2, v ? 0x80 : 0);
                t_str(E, R2, R4, OFF_V);
                t_mov32(E, R2, c ? 0x100 : 0);
                t_str(E, R2, R4, OFF_C);
                t->f.pend = F_NONE;
                t->f.r9kind = F_NONE;
                return;
        }
        t_mov32(E, R9, (n ? 0x80000000u : 0) | (z ? 0x40000000u : 0));
        t->f.apsr = 0;
        t->f.pend = F_LOGIC;
        t->f.xpend = 0;
        t->f.r9kind = F_NONE;
}

/* N/Z of value in rv at `size`, V = C = 0.  Doesn't touch rv. */
static void emit_flags_logic_of(tctx_t *t, int size, int rv)
{
        if (size == 4)
                t_tst(E, rv, rv);
        else
                t_movsh(E, 1, R1, rv, SH_LSL, 32 - 8 * size);
        emit_flags(t, F_LOGIC, 0);
}

/* ARM condition equivalent to 68k condition cc given r9 from an op of
 * `kind`, or -1 if there isn't one.
 */
static int arm_cond_for(int kind, int cc)
{
        static const signed char sub[16] = { C_AL, -1, C_HI, C_LS, C_CS, C_CC, C_NE, C_EQ,
                                             C_VC, C_VS, C_PL, C_MI, C_GE, C_LT, C_GT, C_LE };
        static const signed char add[16] = { C_AL, -1, -1, -1, C_CC, C_CS, C_NE, C_EQ,
                                             C_VC, C_VS, C_PL, C_MI, C_GE, C_LT, C_GT, C_LE };
        static const signed char logic[16] = { C_AL, -1, -1, -1, -1, -1, C_NE, C_EQ,
                                               -1, -1, C_PL, C_MI, C_PL, C_MI, -1, -1 };
        static const signed char shift[16] = { C_AL, -1, -1, -1, C_CC, C_CS, C_NE, C_EQ,
                                               -1, -1, C_PL, C_MI, C_PL, C_MI, -1, -1 };
        switch (kind) {
        case F_SUB:     return sub[cc];
        case F_ADD:     return add[cc];
        case F_LOGIC:   return logic[cc];
        case F_SHIFT:   return shift[cc];
        }
        return -1;
}

/* APSR = r9, unless nothing has touched the flags since they matched */
static void emit_load_apsr(tctx_t *t)
{
        if (t->f.apsr == E->fclob + 1)
                return;
        t_msr_apsr(E, R9);
        t->f.apsr = E->fclob + 1;
}

/* Evaluate 68k condition cc; returns the ARM condition that means "true".
 * (APSR holds the result; flags may still be lazy in r9.)
 */
static int emit_cond(tctx_t *t, int cc)
{
        if (cc == 0)
                return C_AL;
        /* Flags still lazy in r9 and the condition maps onto ARM's: test
         * them directly, and leave storing them to whichever exit needs it.
         */
        if (t->f.pend != F_NONE) {
                int ac = arm_cond_for(t->f.pend, cc);
                if (ac >= 0) {
                        emit_load_apsr(t);
                        return ac;
                }
        }
        emit_flush_flags(t);
        int ac = arm_cond_for(t->f.r9kind, cc);
        if (ac >= 0) {
                emit_load_apsr(t);
                return ac;
        }
        switch (cc) {
        case 2: case 3:                 /* HI / LS: C || Z */
                t_ldr(E, R1, R4, OFF_C);
                t_andi(E, R1, R1, 0x100);
                t_ldr(E, R2, R4, OFF_Z);
                t_clz(E, R2, R2);
                t_movsh(E, 0, R2, R2, SH_LSR, 5);
                t_orrs(E, R1, R1, R2);
                return cc == 2 ? C_EQ : C_NE;
        case 4: case 5:
                t_ldr(E, R1, R4, OFF_C);
                t_tsti(E, R1, 0x100);
                return cc == 5 ? C_NE : C_EQ;
        case 6: case 7:
                t_ldr(E, R1, R4, OFF_Z);
                t_cmpi(E, R1, 0);
                return cc == 7 ? C_EQ : C_NE;
        case 8: case 9:
                t_ldr(E, R1, R4, OFF_V);
                t_tsti(E, R1, 0x80);
                return cc == 9 ? C_NE : C_EQ;
        case 10: case 11:
                t_ldr(E, R1, R4, OFF_N);
                t_tsti(E, R1, 0x80);
                return cc == 11 ? C_NE : C_EQ;
        case 12: case 13:
                t_ldr(E, R1, R4, OFF_N);
                t_ldr(E, R2, R4, OFF_V);
                t_eor(E, R1, R1, R2);
                t_tsti(E, R1, 0x80);
                return cc == 13 ? C_NE : C_EQ;
        case 14: case 15:               /* GT / LE: (N^V) || Z */
                t_ldr(E, R1, R4, OFF_N);
                t_ldr(E, R2, R4, OFF_V);
                t_eor(E, R1, R1, R2);
                t_andi(E, R1, R1, 0x80);
                t_ldr(E, R2, R4, OFF_Z);
                t_clz(E, R2, R2);
                t_movsh(E, 0, R2, R2, SH_LSR, 5);
                t_orrs(E, R1, R1, R2);
                return cc == 15 ? C_NE : C_EQ;
        }
        return -1;      /* F: never */
}

static int invert_cond(int c)
{
        return c ^ 1;
}

/* -------------------------------------------------------------------- */
/* Effective addresses                                                  */

enum { EA_D = 0, EA_A = 1, EA_AI = 2, EA_PI = 3, EA_PD = 4, EA_DI = 5, EA_IX = 6, EA_7 = 7 };

/* Validity classes */
#define V_DREG  0x001
#define V_AREG  0x002
#define V_AI    0x004
#define V_PI    0x008
#define V_PD    0x010
#define V_DI    0x020
#define V_IX    0x040
#define V_AW    0x080
#define V_AL    0x100
#define V_PCDI  0x200
#define V_PCIX  0x400
#define V_IMM   0x800
#define V_MEMALT (V_AI | V_PI | V_PD | V_DI | V_IX | V_AW | V_AL)
#define V_DALT  (V_DREG | V_MEMALT)
#define V_ALT   (V_DALT | V_AREG)
#define V_DATA  (V_DALT | V_PCDI | V_PCIX | V_IMM)
#define V_ALL   (V_DATA | V_AREG)
#define V_CTRL  (V_AI | V_DI | V_IX | V_AW | V_AL | V_PCDI | V_PCIX)

static int ea_class(int mode, int reg)
{
        if (mode < 7)
                return 1 << mode;
        switch (reg) {
        case 0: return V_AW;
        case 1: return V_AL;
        case 2: return V_PCDI;
        case 3: return V_PCIX;
        case 4: return V_IMM;
        }
        return 0;
}

static int ea_ok(int mode, int reg, int allowed)
{
        return (ea_class(mode, reg) & allowed) != 0;
}

/* rd += v for an address: 16-bit forms where they fit.  Those set the
 * ARM flags, which is fine while computing an operand's address: no
 * instruction has flags in flight at that point (the encoder notes the
 * clobber, so a later Bcc reloads APSR).
 */
static void ea_add(tctx_t *t, int rd, int32_t v)
{
        if (rd < 8 && v > 0 && v < 256)
                t16(E, 0x3000 | rd << 8 | v);           /* adds rd, #v */
        else if (rd < 8 && v < 0 && v > -256)
                t16(E, 0x3800 | rd << 8 | -v);          /* subs rd, #-v */
        else
                add_const(t, rd, rd, v);
}

/* Index register for brief extension word, into rd (clobbers r2) */
static void emit_index(tctx_t *t, int rd, uint32_t ext)
{
        int xn = ext >> 12 & 15;
        t_ldr(E, R2, R4, JR_OFF(dar) + 4 * xn);
        if (!(ext & 0x800))
                t_sxth(E, R2, R2);
        t16(E, 0x4400 | (rd & 8) << 4 | R2 << 3 | (rd & 7));  /* add rd, r2 */
        ea_add(t, rd, (int8_t)(ext & 0xff));
}

static void emit_ea_addr_in(tctx_t *t, int mode, int reg, int size, int rd);

/* Compute the address of a memory operand into rd (not r1-r3).
 * Applies (An)+ / -(An) register updates.  For a high register, the
 * address-register modes work in r0 (16-bit forms) and copy the result:
 * that clobbers r0, which then holds the address too.
 */
static void emit_ea_addr(tctx_t *t, int mode, int reg, int size, int rd)
{
        if (rd >= 8 && mode != EA_7) {
                emit_ea_addr_in(t, mode, reg, size, R0);
                t_mov(E, rd, R0);
        } else {
                emit_ea_addr_in(t, mode, reg, size, rd);
        }
}

static void emit_ea_addr_in(tctx_t *t, int mode, int reg, int size, int rd)
{
        int step = (size == 1 && reg == 7) ? 2 : size;
        switch (mode) {
        case EA_AI:
                t_ldr(E, rd, R4, OFF_A(reg));
                break;
        case EA_PI:
                t_ldr(E, rd, R4, OFF_A(reg));
                if (rd < 8)
                        t16(E, 0x1C00 | step << 6 | rd << 3 | R2);      /* adds r2, rd, #step */
                else
                        t_addi(E, R2, rd, step);
                t_str(E, R2, R4, OFF_A(reg));
                break;
        case EA_PD:
                t_ldr(E, rd, R4, OFF_A(reg));
                ea_add(t, rd, -step);
                t_str(E, rd, R4, OFF_A(reg));
                break;
        case EA_DI:
                t_ldr(E, rd, R4, OFF_A(reg));
                ea_add(t, rd, (int16_t)fetch16(t));
                break;
        case EA_IX: {
                uint32_t ext = fetch16(t);
                t_ldr(E, rd, R4, OFF_A(reg));
                emit_index(t, rd, ext);
                break;
        }
        case EA_7:
                switch (reg) {
                case 0:
                        t_mov32(E, rd, (uint32_t)(int16_t)fetch16(t));
                        break;
                case 1:
                        t_mov32(E, rd, fetch32(t));
                        break;
                case 2: {
                        uint32_t base = t->fetch;
                        t_mov32(E, rd, base + (int16_t)fetch16(t));
                        break;
                }
                case 3: {
                        uint32_t base = t->fetch;
                        uint32_t ext = fetch16(t);
                        t_mov32(E, rd, base);
                        emit_index(t, rd, ext);
                        break;
                }
                default:
                        t->fail = 1;
                }
                break;
        default:
                t->fail = 1;
        }
}

/* Read an operand into r0 (memory: zero-extended; registers: whole reg).
 * If addr_reg >= 0 and the operand is in memory, its address is left there.
 */
static void emit_ea_read(tctx_t *t, int mode, int reg, int size, int addr_reg)
{
        switch (mode) {
        case EA_D:
                t_ldr(E, R0, R4, OFF_D(reg));
                return;
        case EA_A:
                t_ldr(E, R0, R4, OFF_A(reg));
                return;
        case EA_7:
                if (reg == 4) {
                        uint32_t v = size == 4 ? fetch32(t) : fetch16(t);
                        if (size == 1)
                                v &= 0xff;
                        t_mov32(E, R0, v);
                        return;
                }
                break;
        }
        if (m68k_jit_fused_ea && addr_reg < 0 && (mode == EA_AI || mode == EA_PI || mode == EA_PD)) {
                t_bl_to(E, ea_rd[mode - EA_AI][reg][size]);
                return;
        }
        int ra = addr_reg >= 0 ? addr_reg : R0;  /* (r0: short encodings, no copy) */
        emit_ea_addr(t, mode, reg, size, ra);
        if (mode != EA_7)
                ra = R0;                /* (emit_ea_addr left it in r0 too) */
        emit_load(t, size, ra);
}

/* Write rv (r0/r8/r10/r11) into data register n at `size` */
static void emit_write_dreg(tctx_t *t, int n, int size, int rv)
{
        if (size == 4) {
                t_str(E, rv, R4, OFF_D(n));
        } else {
                t_ldr(E, R3, R4, OFF_D(n));
                t_bfi(E, R3, rv, 0, 8 * size);
                t_str(E, R3, R4, OFF_D(n));
        }
}

/* -------------------------------------------------------------------- */
/* Block structure                                                      */

static void emit_prologue(tctx_t *t)
{
        t16(E, 0xB508);                 /* push {r3, lr} (keeps SP 8-aligned) */
}

static int chainable(uint32_t pc)
{
        return (pc & 0xff000000) == 0 && fetchable(pc);
}

/* Charge `ninstr` against the budget in r7; leaves ARM flags from the subtract */
static void emit_charge(tctx_t *t, int ninstr)
{
        t16(E, 0x3F00 | ninstr);                /* subs r7, #ninstr */
}

/* Does the 68k instruction op (as this translator handles it, when it's
 * the first of a block) overwrite the flags before anything can read
 * them or leave the block?  KF_NZVC: N, Z, V and C; KF_X: X as well.
 */
#define KF_NZVC 1
#define KF_X    2
static int kills_flags(uint32_t op)
{
        int sz = op >> 6 & 3, mode = op >> 3 & 7, reg = op & 7, opmode = op >> 6 & 7;
        switch (op >> 12) {
        case 0x1: case 0x2: case 0x3:                   /* MOVE (not MOVEA) */
                return opmode == 1 ? 0 : KF_NZVC;
        case 0x7:                                       /* MOVEQ */
                return op & 0x100 ? 0 : KF_NZVC;
        case 0x4:
                if (sz != 3 && ((op & 0xff00) == 0x4a00 || (op & 0xff00) == 0x4200 ||
                                (op & 0xff00) == 0x4600))       /* TST, CLR, NOT */
                        return KF_NZVC;
                if (sz != 3 && (op & 0xff00) == 0x4400)         /* NEG */
                        return KF_NZVC | KF_X;
                if ((op & 0xfff8) == 0x4840 || (op & 0xfff8) == 0x4880 || (op & 0xfff8) == 0x48c0)
                        return KF_NZVC;                         /* SWAP, EXT */
                return 0;
        case 0x0:                                       /* ORI ANDI SUBI ADDI EORI CMPI */
                if ((op & 0x100) || (op & 0xf00) == 0x800 || sz == 3 || (mode == 7 && reg == 4))
                        return 0;
                switch (op >> 9 & 7) {
                case 0: case 1: case 5: case 6: return KF_NZVC;
                case 2: case 3: return KF_NZVC | KF_X;
                }
                return 0;
        case 0x5:                                       /* ADDQ/SUBQ, not to An */
                return sz == 3 || mode == 1 ? 0 : KF_NZVC | KF_X;
        case 0x8:                                       /* OR (not DIV, SBCD) */
                return opmode < 3 || ((opmode & 3) != 3 && mode >= 2) ? KF_NZVC : 0;
        case 0xc:                                       /* AND, MUL (not ABCD, EXG) */
                return opmode < 4 || opmode == 7 || mode >= 2 ? KF_NZVC : 0;
        case 0x9: case 0xd:                             /* ADD/SUB (not ADDA, ADDX) */
                return opmode < 3 || ((opmode & 3) != 3 && mode >= 2) ? KF_NZVC | KF_X : 0;
        case 0xb:                                       /* CMP, CMPA, CMPM, EOR */
                return KF_NZVC;
        case 0xe:                                       /* LSd/ASd #n,Dn */
                return sz != 3 && !(op & 0x20) && (op >> 3 & 3) <= 1 ? KF_NZVC | KF_X : 0;
        }
        return 0;
}

/* kills_flags for the block at pc.  Not for a native ROM routine's
 * block: natives read the flags from the register file (SR), so they
 * have to be stored.
 */
static int target_kills_flags(uint32_t pc)
{
        if (!m68k_jit_no_native && m68k_native_lookup(pc) >= 0)
                return 0;
        return kills_flags(peek16(pc));
}

/* Leave the block for 68k address pc after `ninstr` instructions.  Exits
 * to ROM get a chain slot: a branch that starts out going to the exit stub
 * and gets patched to jump straight into the next block.
 */
static void emit_exit_const(tctx_t *t, uint32_t pc, int ninstr)
{
        fstate_t saved = t->f;
        if (m68k_jit_inline_rts && fetchable(pc) && peek16(pc) == 0x4e75 &&
            (m68k_jit_no_native || m68k_native_lookup(pc) < 0) && add_word_dep(t, pc)) {
                /* Leaving for an RTS: do the RTS here instead of jumping
                 * to a block that just does it.
                 */
                emit_flush_flags(t);
                emit_charge(t, ninstr + 1);
                t_b_to(E, C_AL, rts_fn);
                t->f = saved;
                return;
        }
        int chain = chainable(pc) && t->nstubs < MAX_STUBS && nexits < MAX_EXITS;
        int need = 0;
        if (chain && m68k_jit_exit_flag_elide && (t->f.pend != F_NONE || t->f.xpend)) {
                /* Chained into a block that overwrites the flags first
                 * thing: don't store them on the way.  (The exit stub
                 * does, for the trip back to the dispatcher; linking
                 * checks the target block still starts that way.)
                 */
                int k = target_kills_flags(pc);
                if ((k & KF_NZVC) && (!t->f.xpend || (k & KF_X)))
                        need = KF_NZVC | (t->f.xpend ? KF_X : 0);
                else if ((k & KF_NZVC) && t->f.pend != F_NONE)
                        need = KF_NZVC;         /* X still has to be stored */
        }
        fstate_t unstored = t->f;
        if (!need)
                emit_flush_flags(t);
        else if (!(need & KF_X) && t->f.xpend)
                emit_store_x(t, t->f.pend);
        int fdefer = 0;
        if (chain && need && E->defer && pc > t->pc) {
                /* Flags still only in APSR, and the chained path never
                 * needs them: charge without touching APSR (sub, not
                 * subs) and let the stub capture them.
                 */
                fdefer = E->defer;
                E->defer = 0;
                t_subi(E, R7, R7, ninstr);
        } else {
                emit_charge(t, ninstr);
        }
        if (chain) {
                stub_t *st = &t->stubs[t->nstubs++];
                st->type = ST_EXIT;
                st->pc = pc;
                st->need = need;
                st->fdefer = fdefer;
                st->fpend = unstored.pend;
                st->fxpend = unstored.xpend;
                st->fr9kind = unstored.r9kind;
                /* Only backward exits (loops) check the budget: anything that
                 * runs forever has to come back round through one of those,
                 * or through a computed jump, which checks too.
                 */
                if (pc <= t->pc) {
                        st->from = t_b_placeholder(E, C_LE);    /* out of budget */
                } else {
                        st->from.at = NULL;
                }
                st->from2 = t_b_placeholder(E, C_AL);   /* the chain slot */
                st->ret = NULL;
        } else {
                t_mov32(E, R0, pc);
                t_str(E, R0, R4, OFF_PC);
                t16(E, 0xBD08);                 /* pop {r3, pc} */
        }
        t->f = saved;                   /* other exits may still need flushing */
}

static void emit_exit_reg(tctx_t *t, int rpc, int ninstr)
{
        fstate_t saved = t->f;
        emit_flush_flags(t);
        t_str(E, rpc, R4, OFF_PC);
        emit_charge(t, ninstr);
        /* Budget left: try the jump cache.  (B<cond>.W only reaches 1MB,
         * so branch over an unconditional B.W, which reaches 16MB.)
         */
        tbr_t done = t_b_placeholder_n(E, C_LE);
        t_b_to(E, C_AL, dyn_jump);
        t_patch_branch(done, E->p);
        t->f.apsr = 0;
        t16(E, 0xBD08);                         /* pop {r3, pc} */
        t->f = saved;
}

/* Push the long in rv (not r0; r1 saves a copy) onto the 68k stack */
static void emit_push32(tctx_t *t, int rv, int last_action)
{
        t_ldr(E, R0, R4, OFF_A(7));
        ea_add(t, R0, -4);
        t_str(E, R0, R4, OFF_A(7));
        if (last_action)
                emit_store(t, 4, R0, rv);
        else
                emit_store_nochk(t, 4, R0, rv);
}

/* -------------------------------------------------------------------- */
/* Instruction translation.                                             */
/* Each returns: 0 = translated, continue; 1 = translated, block ended;  */
/* -1 = not supported (nothing emitted).                                */

#define SIZE_BWL(s)     ((s) == 0 ? 1 : (s) == 1 ? 2 : (s) == 2 ? 4 : 0)

/* dst(r0) OP src(r8) at size -> r0, ARM flags set */
enum { OP_ADD, OP_SUB, OP_AND, OP_OR, OP_EOR, OP_CMP };

static void emit_alu(tctx_t *t, int op, int size)
{
        int dp = op == OP_ADD ? DP_ADD : op == OP_AND ? DP_AND : op == OP_OR ? DP_ORR :
                 op == OP_EOR ? DP_EOR : DP_SUB;
        int rd = op == OP_CMP ? R1 : R0;
        if (t->alu_imm_valid) {
                /* Immediate source: encode it in the instruction if it fits
                 * (moved to the top of the register for byte/word sizes).
                 */
                t->alu_imm_valid = 0;
                int sh = 32 - 8 * size;
                uint32_t imm = sh == 32 ? 0 : t->alu_imm << sh;
                if (size == 4)
                        imm = t->alu_imm;
                if (t_modimm(imm) >= 0) {
                        if (size == 4) {
                                t_dpi(E, dp, 1, rd, R0, imm);
                        } else {
                                t_movsh(E, 0, R1, R0, SH_LSL, sh);
                                t_dpi(E, dp, 1, rd, R1, imm);
                                if (rd == R0)
                                        t_movsh(E, 0, R0, R0, SH_LSR, sh);
                        }
                        return;
                }
                t_mov32(E, R8, t->alu_imm);
        }
        if (size == 4) {
                t_dpr(E, dp, 1, rd, R0, R8, SH_LSL, 0);
        } else {
                int sh = 32 - 8 * size;
                t_movsh(E, 0, R1, R0, SH_LSL, sh);
                t_dpr(E, dp, 1, rd, R1, R8, SH_LSL, sh);
                if (rd == R0)
                        t_movsh(E, 0, R0, R0, SH_LSR, sh);
        }
}

static int alu_flag_kind(int op)
{
        return op == OP_ADD ? F_ADD : (op == OP_SUB || op == OP_CMP) ? F_SUB : F_LOGIC;
}

/* <dst ea> = <dst ea> OP src(r8); handles Dn and memory destinations */
static void emit_alu_to_ea(tctx_t *t, int op, int size, int mode, int reg)
{
        if (mode == EA_D) {
                t_ldr(E, R0, R4, OFF_D(reg));
                emit_alu(t, op, size);
                emit_flags(t, alu_flag_kind(op), op == OP_ADD || op == OP_SUB);
                if (op != OP_CMP)
                        emit_write_dreg(t, reg, size, R0);
        } else {
                emit_ea_read(t, mode, reg, size, R10);
                emit_alu(t, op, size);
                emit_flags(t, alu_flag_kind(op), op == OP_ADD || op == OP_SUB);
                if (op != OP_CMP)
                        emit_store(t, size, R10, R0);
        }
}

static int tr_move(tctx_t *t, uint32_t op)
{
        int size = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
        int sm = op >> 3 & 7, sr = op & 7, dm = op >> 6 & 7, dr = op >> 9 & 7;
        if (!ea_ok(sm, sr, size == 1 ? V_DATA : V_ALL))
                return -1;
        if (dm == EA_A) {                               /* MOVEA */
                if (size == 1)
                        return -1;
                emit_ea_read(t, sm, sr, size, -1);
                if (size == 2)
                        t_sxth(E, R0, R0);
                t_str(E, R0, R4, OFF_A(dr));
                return 0;
        }
        if (!ea_ok(dm, dr, V_DALT))
                return -1;
        emit_ea_read(t, sm, sr, size, -1);
        if (dm == EA_D) {
                emit_flags_logic_of(t, size, R0);
                emit_write_dreg(t, dr, size, R0);
                return 0;
        }
        emit_flags_logic_of(t, size, R0);       /* (clobbers r1) */
        t_mov(E, R1, R0);
        if (m68k_jit_fused_ea && (dm == EA_AI || dm == EA_PI || dm == EA_PD)) {
                t_bl_to(E, ea_wr[dm - EA_AI][dr][size]);
                emit_smc_check(t);
                return 0;
        }
        /* Value in r1, address straight into r0, as the store wants them */
        emit_ea_addr(t, dm, dr, size, R0);
        emit_store(t, size, R0, R1);
        return 0;
}

static int tr_moveq(tctx_t *t, uint32_t op)
{
        if (op & 0x100)
                return -1;
        int32_t v = (int8_t)(op & 0xff);
        /* N and Z from a flag-setting move; V and C come out as 0 for
         * F_LOGIC whatever ARM's are
         */
        if (v >= 0) {
                t_movs8(E, R0, v);
        } else {
                t_movs8(E, R0, ~v);
                t16(E, 0x43C0 | R0 << 3 | R0);  /* mvns r0, r0 */
        }
        emit_flags(t, F_LOGIC, 0);
        t_str(E, R0, R4, OFF_D(op >> 9 & 7));
        return 0;
}

/* ADD/SUB/AND/OR/CMP/EOR and their A-register forms (lines 8,9,B,C,D) */
static int tr_alu(tctx_t *t, uint32_t op, int line)
{
        int reg = op >> 9 & 7, opmode = op >> 6 & 7, mode = op >> 3 & 7, r = op & 7;
        int aop = line == 0xD ? OP_ADD : line == 0x9 ? OP_SUB : line == 0xC ? OP_AND :
                  line == 0x8 ? OP_OR : OP_CMP;

        if (opmode == 3 || opmode == 7) {
                /* ADDA / SUBA / CMPA (AND/OR: MULU/MULS/DIVU/DIVS, handled elsewhere) */
                if (aop != OP_ADD && aop != OP_SUB && aop != OP_CMP)
                        return -1;
                int size = opmode == 3 ? 2 : 4;
                if (!ea_ok(mode, r, V_ALL))
                        return -1;
                emit_ea_read(t, mode, r, size, -1);
                if (size == 2)
                        t_sxth(E, R8, R0);
                else
                        t_mov(E, R8, R0);
                t_ldr(E, R0, R4, OFF_A(reg));
                if (aop == OP_CMP) {
                        t_subs(E, R1, R0, R8);
                        emit_flags(t, F_SUB, 0);
                } else {
                        if (aop == OP_ADD)
                                t_add(E, R0, R0, R8);
                        else
                                t_sub(E, R0, R0, R8);
                        t_str(E, R0, R4, OFF_A(reg));
                }
                return 0;
        }

        int size = SIZE_BWL(opmode & 3);
        if (opmode < 3) {
                /* <ea> OP Dn -> Dn */
                int allowed = (aop == OP_AND || aop == OP_OR) ? V_DATA : V_ALL;
                if (size == 1)
                        allowed &= ~V_AREG;
                if (!ea_ok(mode, r, allowed))
                        return -1;
                emit_ea_read(t, mode, r, size, -1);
                t_mov(E, R8, R0);
                emit_alu_to_ea(t, aop, size, EA_D, reg);
                return 0;
        }

        /* Dn OP <ea> -> <ea>  (line B with opmode 4-6 is EOR) */
        if (line == 0xB) {
                if (mode == EA_A)
                        return -1;              /* CMPM */
                aop = OP_EOR;
                if (!ea_ok(mode, r, V_DALT))
                        return -1;
        } else {
                if (mode == EA_D || mode == EA_A)
                        return -1;              /* ADDX/SUBX/ABCD/SBCD/EXG */
                if (!ea_ok(mode, r, V_MEMALT))
                        return -1;
        }
        t_ldr(E, R8, R4, OFF_D(reg));
        emit_alu_to_ea(t, aop, size, mode, r);
        return 0;
}

static int tr_cmpm(tctx_t *t, uint32_t op)
{
        int size = SIZE_BWL(op >> 6 & 3);
        int ax = op >> 9 & 7, ay = op & 7;
        emit_ea_read(t, EA_PI, ay, size, -1);
        t_mov(E, R8, R0);
        emit_ea_read(t, EA_PI, ax, size, -1);
        emit_alu(t, OP_CMP, size);
        emit_flags(t, F_SUB, 0);
        return 0;
}

/* ORI/ANDI/SUBI/ADDI/EORI/CMPI #imm,<ea> */
static int tr_immop(tctx_t *t, uint32_t op)
{
        int kind = op >> 9 & 7, sz = op >> 6 & 3, mode = op >> 3 & 7, r = op & 7;
        static const int ops[8] = { OP_OR, OP_AND, OP_SUB, OP_ADD, -1, OP_EOR, OP_CMP, -1 };
        int aop = ops[kind];
        if (aop < 0 || sz == 3)
                return -1;
        if (!ea_ok(mode, r, V_DALT))
                return -1;                      /* incl. to CCR/SR */
        int size = SIZE_BWL(sz);
        uint32_t imm = size == 4 ? fetch32(t) : fetch16(t);
        if (size == 1)
                imm &= 0xff;
        t->alu_imm_valid = 1;
        t->alu_imm = imm;
        emit_alu_to_ea(t, aop, size, mode, r);
        return 0;
}

static int tr_addq_subq(tctx_t *t, uint32_t op)
{
        int data = op >> 9 & 7, size = SIZE_BWL(op >> 6 & 3), mode = op >> 3 & 7, r = op & 7;
        int aop = (op & 0x100) ? OP_SUB : OP_ADD;
        if (!data)
                data = 8;
        if (mode == EA_A) {
                if (size == 1)
                        return -1;
                t_ldr(E, R0, R4, OFF_A(r));
                if (aop == OP_ADD)
                        t_addi(E, R0, R0, data);
                else
                        t_subi(E, R0, R0, data);
                t_str(E, R0, R4, OFF_A(r));
                return 0;
        }
        if (!ea_ok(mode, r, V_DALT))
                return -1;
        t->alu_imm_valid = 1;
        t->alu_imm = data;
        emit_alu_to_ea(t, aop, size, mode, r);
        return 0;
}

/* CLR / NEG / NOT / TST */
static int tr_unary(tctx_t *t, uint32_t op, int kind)
{
        int sz = op >> 6 & 3, mode = op >> 3 & 7, r = op & 7;
        if (sz == 3 || !ea_ok(mode, r, V_DALT))
                return -1;
        int size = SIZE_BWL(sz), sh = 32 - 8 * size;

        if (kind == 0) {                        /* CLR */
                /* (flags first: the store must be the last thing we do) */
                if (mode == EA_D) {
                        t_movs8(E, R0, 0);
                        emit_flags(t, F_LOGIC, 0);
                        emit_write_dreg(t, r, size, R0);
                } else {
                        emit_ea_addr(t, mode, r, size, R0);
                        emit_flags_const(t, 0, 1, 0, 0);
                        t_movs8(E, R1, 0);      /* (APSR is free: the flags are in r9) */
                        emit_store(t, size, R0, R1);
                }
                return 0;
        }

        emit_ea_read(t, mode, r, size, R10);
        if (kind == 3) {                        /* TST */
                emit_flags_logic_of(t, size, R0);
                return 0;
        }
        if (kind == 1) {                        /* NEG */
                if (size == 4) {
                        t_rsbsi(E, R0, R0, 0);
                } else {
                        t_movsh(E, 0, R1, R0, SH_LSL, sh);
                        t_rsbsi(E, R0, R1, 0);
                        t_movsh(E, 0, R0, R0, SH_LSR, sh);
                }
                emit_flags(t, F_SUB, 1);
        } else {                                /* NOT */
                t_mvn(E, R1, R0);
                if (size == 4) {
                        t_mov(E, R0, R1);
                        t_tst(E, R0, R0);
                } else {
                        t_movsh(E, 1, R0, R1, SH_LSL, sh);
                        t_movsh(E, 0, R0, R0, SH_LSR, sh);
                }
                emit_flags(t, F_LOGIC, 0);
        }
        if (mode == EA_D)
                emit_write_dreg(t, r, size, R0);
        else
                emit_store(t, size, R10, R0);
        return 0;
}

static int tr_lea(tctx_t *t, uint32_t op)
{
        int mode = op >> 3 & 7, r = op & 7;
        if (!ea_ok(mode, r, V_CTRL))
                return -1;
        emit_ea_addr(t, mode, r, 4, R0);
        t_str(E, R0, R4, OFF_A(op >> 9 & 7));
        return 0;
}

static int tr_pea(tctx_t *t, uint32_t op)
{
        int mode = op >> 3 & 7, r = op & 7;
        if (!ea_ok(mode, r, V_CTRL))
                return -1;
        emit_ea_addr(t, mode, r, 4, R0);
        t_mov(E, R1, R0);
        emit_push32(t, R1, 1);
        return 0;
}

static int tr_ext_swap(tctx_t *t, uint32_t op)
{
        int r = op & 7;
        t_ldr(E, R0, R4, OFF_D(r));
        switch (op & 0xfff8) {
        case 0x4840:                            /* SWAP */
                t_movsh(E, 0, R0, R0, SH_ROR, 16);
                t_str(E, R0, R4, OFF_D(r));
                emit_flags_logic_of(t, 4, R0);
                return 0;
        case 0x4880:                            /* EXT.W */
                t_sxtb(E, R0, R0);
                emit_write_dreg(t, r, 2, R0);
                emit_flags_logic_of(t, 2, R0);
                return 0;
        case 0x48c0:                            /* EXT.L */
                t_sxth(E, R0, R0);
                t_str(E, R0, R4, OFF_D(r));
                emit_flags_logic_of(t, 4, R0);
                return 0;
        }
        return -1;
}

static int tr_movem(tctx_t *t, uint32_t op)
{
        int to_regs = op & 0x400, lng = op & 0x40;
        int mode = op >> 3 & 7, r = op & 7;
        uint32_t mask = fetch16(t);
        uint32_t spec = mask | (lng ? MV_LONG : 0) | (uint32_t)r << 20;

        if (to_regs) {
                if (!ea_ok(mode, r, V_CTRL | V_PI))
                        return -1;
                spec |= MV_TOREGS;
                if (mode == EA_PI) {
                        spec |= MV_AUTO;
                        t_ldr(E, R0, R4, OFF_A(r));
                } else {
                        emit_ea_addr(t, mode, r, lng ? 4 : 2, R0);
                }
        } else {
                if (!ea_ok(mode, r, (V_CTRL & ~(V_PCDI | V_PCIX)) | V_PD))
                        return -1;
                if (mode == EA_PD) {
                        spec |= MV_AUTO;
                        t_ldr(E, R0, R4, OFF_A(r));
                } else {
                        emit_ea_addr(t, mode, r, lng ? 4 : 2, R0);
                }
        }
        t_mov32(E, R1, spec);
        call_helper(t, JH_MOVEM);
        return 0;
}

static int tr_link(tctx_t *t, uint32_t op)
{
        int r = op & 7;
        int16_t d = fetch16(t);
        t_ldr(E, R0, R4, OFF_A(7));
        ea_add(t, R0, -4);
        t_str(E, R0, R4, OFF_A(7));
        if (r == 7)
                t_mov(E, R1, R0);               /* LINK A7 pushes the new SP */
        else
                t_ldr(E, R1, R4, OFF_A(r));
        emit_store_nochk(t, 4, R0, R1);
        t_ldr(E, R0, R4, OFF_A(7));
        if (r != 7)
                t_str(E, R0, R4, OFF_A(r));
        add_const(t, R0, R0, d);
        t_str(E, R0, R4, OFF_A(7));
        return 0;
}

static int tr_unlk(tctx_t *t, uint32_t op)
{
        int r = op & 7;
        t_ldr(E, R10, R4, OFF_A(r));
        emit_load(t, 4, R10);
        if (r == 7) {
                t_str(E, R0, R4, OFF_A(7));
        } else {
                t_addi(E, R2, R10, 4);
                t_str(E, R2, R4, OFF_A(7));
                t_str(E, R0, R4, OFF_A(r));
        }
        return 0;
}

static int tr_exg(tctx_t *t, uint32_t op)
{
        int rx = op >> 9 & 7, ry = op & 7, offx, offy;
        switch (op & 0x1f8) {
        case 0x140: offx = OFF_D(rx); offy = OFF_D(ry); break;
        case 0x148: offx = OFF_A(rx); offy = OFF_A(ry); break;
        case 0x188: offx = OFF_D(rx); offy = OFF_A(ry); break;
        default: return -1;
        }
        t_ldr(E, R0, R4, offx);
        t_ldr(E, R1, R4, offy);
        t_str(E, R1, R4, offx);
        t_str(E, R0, R4, offy);
        return 0;
}

static int tr_mul(tctx_t *t, uint32_t op)
{
        int is_signed = op & 0x100, mode = op >> 3 & 7, r = op & 7, dn = op >> 9 & 7;
        if (!ea_ok(mode, r, V_DATA))
                return -1;
        emit_ea_read(t, mode, r, 2, -1);
        if (is_signed)
                t_sxth(E, R8, R0);
        else
                t_uxth(E, R8, R0);
        t_ldr(E, R0, R4, OFF_D(dn));
        if (is_signed)
                t_sxth(E, R0, R0);
        else
                t_uxth(E, R0, R0);
        t_mul(E, R0, R0, R8);
        t_str(E, R0, R4, OFF_D(dn));
        emit_flags_logic_of(t, 4, R0);
        return 0;
}

/* BTST/BCHG/BCLR/BSET, dynamic (Dn) and static (#imm) bit number */
static int tr_bitop(tctx_t *t, uint32_t op)
{
        int kind = op >> 6 & 3, mode = op >> 3 & 7, r = op & 7;
        int dynamic = op & 0x100;
        if (mode == EA_A)
                return -1;                      /* MOVEP */
        int allowed = kind == 0 ? (V_DATA & ~V_IMM) : V_DALT;
        if (!dynamic && (op & 0xe00) != 0x800)
                return -1;
        if (!ea_ok(mode, r, allowed))
                return -1;

        int size = mode == EA_D ? 4 : 1;
        if (!dynamic) {
                /* Bit number known: extract / flip it with immediates */
                int bit = fetch16(t) & (size == 4 ? 31 : 7);
                emit_ea_read(t, mode, r, size, R10);
                emit_flush_flags(t);
                t->f.r9kind = F_NONE;
                t_ubfx(E, R1, R0, bit, 1);
                t_str(E, R1, R4, OFF_Z);
                if (kind == 0)
                        return 0;
                t_dpi(E, kind == 1 ? DP_EOR : kind == 2 ? DP_BIC : DP_ORR, 0, R0, R0, 1u << bit);
                if (mode == EA_D)
                        t_str(E, R0, R4, OFF_D(r));
                else
                        emit_store(t, 1, R10, R0);
                return 0;
        }
        t_ldr(E, R8, R4, OFF_D(op >> 9 & 7));
        t_andi(E, R8, R8, size == 4 ? 31 : 7);
        emit_ea_read(t, mode, r, size, R10);
        /* Only Z changes: make the others current first */
        emit_flush_flags(t);
        t->f.r9kind = F_NONE;
        /* not_z = the bit (nonzero = Z clear) */
        t_shr(E, SH_LSR, 0, R1, R0, R8);
        t_andi(E, R1, R1, 1);
        t_str(E, R1, R4, OFF_Z);
        if (kind == 0)
                return 0;
        t_mov32(E, R1, 1);
        t_shr(E, SH_LSL, 0, R1, R1, R8);
        if (kind == 1)
                t_eor(E, R0, R0, R1);
        else if (kind == 2)
                t_dpr(E, DP_BIC, 0, R0, R0, R1, SH_LSL, 0);
        else
                t_orr(E, R0, R0, R1);
        if (mode == EA_D)
                t_str(E, R0, R4, OFF_D(r));
        else
                emit_store(t, 1, R10, R0);
        return 0;
}

/* LSL/LSR/ASR with immediate count, on a data register */
static int tr_shift(tctx_t *t, uint32_t op)
{
        int count = op >> 9 & 7, left = op & 0x100, sz = op >> 6 & 3;
        int type = op >> 3 & 3, by_reg = op & 0x20, r = op & 7;
        if (sz == 3 || by_reg)
                return -1;
        if (!count)
                count = 8;
        int size = SIZE_BWL(sz), sh = 32 - 8 * size;
        if (type > 1)
                return -1;                      /* ROXd, ROd */
        if (type == 0 && left) {
                /* ASL: like LSL, but V = the sign bit changed at any point,
                 * i.e. the top count+1 bits weren't all the same.
                 */
                t_ldr(E, R0, R4, OFF_D(r));
                t_movsh(E, 0, R1, R0, SH_LSL, sh);              /* operand at the top */
                t_movsh(E, 1, R0, R1, SH_LSL, count);           /* N, Z, C */
                retire_r9(t, 1);
                E->defer = 0;
                t_mrs_apsr(E, R9);
                t_movsh(E, 0, R2, R1, SH_ASR, 31 - count);      /* top count+1 bits, sign-extended */
                t_addi(E, R2, R2, 1);                           /* 0 or 1 if they were all equal */
                t_bici(E, R9, R9, 0x10000000);
                t->f.apsr = 0;
                t_cmpi(E, R2, 1);
                tbr_t same = t_b_placeholder_n(E, C_LS);
                t_orri(E, R9, R9, 0x10000000);
                t_patch_branch(same, E->p);
                t->f.apsr = 0;
                t->f.pend = F_ADD;              /* V and C taken as they are */
                t->f.xpend = 1;
                t->f.r9kind = F_NONE;
                if (size != 4)
                        t_movsh(E, 0, R0, R0, SH_LSR, sh);
                emit_write_dreg(t, r, size, R0);
                return 0;
        }
        t_ldr(E, R0, R4, OFF_D(r));
        if (left) {
                /* operand at the top of the register so C/N/Z come out right */
                if (size == 4) {
                        t_movsh(E, 1, R0, R0, SH_LSL, count);
                } else {
                        t_movsh(E, 0, R1, R0, SH_LSL, sh);
                        t_movsh(E, 1, R0, R1, SH_LSL, count);
                        t_movsh(E, 0, R0, R0, SH_LSR, sh);
                }
        } else if (type == 1) {                 /* LSR */
                if (size == 1)
                        t_uxtb(E, R0, R0);
                else if (size == 2)
                        t_uxth(E, R0, R0);
                t_movsh(E, 1, R0, R0, SH_LSR, count);
        } else {                                /* ASR */
                if (size == 1)
                        t_sxtb(E, R0, R0);
                else if (size == 2)
                        t_sxth(E, R0, R0);
                t_movsh(E, 1, R0, R0, SH_ASR, count);
        }
        emit_flags(t, F_SHIFT, 1);
        emit_write_dreg(t, r, size, R0);
        return 0;
}

static int tr_scc(tctx_t *t, uint32_t op)
{
        int cc = op >> 8 & 15, mode = op >> 3 & 7, r = op & 7;
        if (!ea_ok(mode, r, V_DALT))
                return -1;
        if (mode != EA_D)
                emit_ea_addr(t, mode, r, 1, R10);
        t_mov32(E, R8, 0);
        if (cc == 0) {
                t_mov32(E, R8, 0xff);
        } else if (cc != 1) {
                int c = emit_cond(t, cc);
                tbr_t skip = t_b_placeholder_n(E, invert_cond(c));
                t_mov32(E, R8, 0xff);
                t_patch_branch(skip, E->p);
                t->f.apsr = 0;
        }
        if (mode == EA_D)
                emit_write_dreg(t, r, 1, R8);
        else
                emit_store(t, 1, R10, R8);
        return 0;
}

/* Control flow: all of these end the block */

/* If ARM condition c holds, leave the block for pc (after n instructions);
 * otherwise fall through and keep going.  The exit sits inline, right
 * after the test (taken: one chained jump to the target; not taken: a
 * short hop over it), so neither path jumps far.
 */
static void emit_cond_exit(tctx_t *t, int c, uint32_t pc, int n)
{
        int apsr_ok = t->f.apsr == E->fclob + 1;
        /* An owed MRS can wait: the exit path emits it if it needs it,
         * and the branch leaves APSR alone for the other path.
         */
        int defer = E->defer;
        E->defer = 0;
        tbr_t skip = t_b_placeholder_n(E, invert_cond(c));
        E->defer = defer;
        emit_exit_const(t, pc, n);
        if (!t_reaches_n(skip, E->p))
                t->fail = 1;            /* (can't happen: the exit is a few instructions) */
        t_patch_branch(skip, E->p);
        E->defer = defer;
        /* Only the branch arrives here, flags untouched */
        t->f.apsr = apsr_ok ? E->fclob + 1 : 0;
}

static int tr_bcc(tctx_t *t, uint32_t op)
{
        int cc = op >> 8 & 15;
        uint32_t base = t->pc + 2;
        int32_t disp = (int8_t)(op & 0xff);
        if ((op & 0xff) == 0)
                disp = (int16_t)fetch16(t);
        else if ((op & 0xff) == 0xff)
                return -1;                      /* 68020 long branch */
        if (t->fail)
                return -1;
        uint32_t target = base + disp, next = t->fetch;
        int n = t->count + 1;

        if (cc == 1) {                          /* BSR */
                t_mov32(E, R1, next);
                emit_push32(t, R1, 0);
                if (can_follow(t, target)) {
                        t->follow = target;
                        return 2;
                }
                emit_exit_const(t, target, n);
                return 1;
        }
        if (cc == 0) {                          /* BRA */
                if (can_follow(t, target)) {
                        t->follow = target;
                        return 2;
                }
                emit_exit_const(t, target, n);
                return 1;
        }
        if (cc == 1 || cc == 0)
                return 1;
        if (m68k_jit_no_traces) {
                int c0 = emit_cond(t, cc);
                tbr_t taken = t_b_placeholder(E, c0);
                emit_exit_const(t, next, n);
                t_patch_branch(taken, E->p);
                t->f.apsr = 0;
                emit_exit_const(t, target, n);
                return 1;
        }
        /* Taken: out-of-line exit.  Not taken: the trace carries on, so hot
         * code stays straight-line (taken jumps into new cache lines cost
         * a PSRAM miss each).
         */
        int c = emit_cond(t, cc);
        if (c < 0)
                return 0;                       /* BF: never taken */
        emit_cond_exit(t, c, target, n);
        return 0;
}

static int tr_dbcc(tctx_t *t, uint32_t op)
{
        int cc = op >> 8 & 15, r = op & 7;
        uint32_t target = t->pc + 2 + (int16_t)fetch16(t), next = t->fetch;
        int n = t->count + 1;
        tbr_t cond_true = { NULL, 0 };
        (void)next;
        if (cc == 0)
                return 0;                       /* DBT: never loops */
        if (cc != 1)
                cond_true = t_b_placeholder_n(E, emit_cond(t, cc));
        t_ldr(E, R0, R4, OFF_D(r));
        t_movsh(E, 1, R1, R0, SH_LSL, 16);      /* Z: the word was 0, so it's about to expire */
        t_subi(E, R2, R0, 1);                   /* (no flags from here on) */
        t_bfi(E, R0, R2, 0, 16);
        t_str(E, R0, R4, OFF_D(r));
        emit_cond_exit(t, C_NE, target, n);     /* not expired: loop */
        if (cond_true.at) {
                if (!t_reaches_n(cond_true, E->p))
                        t->fail = 1;
                t_patch_branch(cond_true, E->p);
                t->f.apsr = 0;
        }
        return 0;                               /* fall through */
}

static int tr_jmp_jsr(tctx_t *t, uint32_t op)
{
        int mode = op >> 3 & 7, r = op & 7, jsr = !(op & 0x40);
        if (!ea_ok(mode, r, V_CTRL))
                return -1;
        if (mode == EA_7 && r <= 2) {
                /* Target known now: abs.W, abs.L or d16(PC) */
                uint32_t base = t->fetch, target;
                if (r == 0)
                        target = (uint32_t)(int16_t)fetch16(t);
                else if (r == 1)
                        target = fetch32(t);
                else
                        target = base + (int16_t)fetch16(t);
                if (t->fail)
                        return -1;
                if (jsr) {
                        t_mov32(E, R1, t->fetch);
                        emit_push32(t, R1, 0);
                }
                if (can_follow(t, target)) {
                        t->follow = target;
                        return 2;
                }
                emit_exit_const(t, target, t->count + 1);
                return 1;
        }
        emit_ea_addr(t, mode, r, 4, R10);
        if (jsr) {
                t_mov32(E, R1, t->fetch);
                emit_push32(t, R1, 0);
        }
        emit_exit_reg(t, R10, t->count + 1);
        return 1;
}

static int tr_rts(tctx_t *t)
{
        emit_flush_flags(t);
        emit_charge(t, t->count + 1);
        t_b_to(E, C_AL, rts_fn);
        return 1;
}

/* Block for a native ROM routine: call it, charge what it says the ROM
 * code would have taken, and go on at the pc it returns.  (Flags are
 * already in Musashi's format at a block start.)
 */
static void emit_native(tctx_t *t, int idx, uint32_t pc)
{
        t->pc = pc;
        t_mov32(E, R0, (uint32_t)idx);
        t_mov32(E, R1, pc);
        call_helper(t, JH_NATIVE);
        t_ldr(E, R1, R4, JR_OFF(native_n));
        t_sub(E, R7, R7, R1);
        t->f.r9kind = F_NONE;
        emit_exit_reg(t, R0, 0);
}

static int tr_aline(tctx_t *t)
{
        emit_flush_flags(t);            /* the helper builds SR from them */
        t_mov32(E, R0, t->pc);
        call_helper(t, JH_ALINE);
        /* (plus the dispatcher's instructions, if it did those natively) */
        t_ldr(E, R1, R4, JR_OFF(native_n));
        t_sub(E, R7, R7, R1);
        emit_exit_reg(t, R0, t->count + 1);
        return 1;
}

/* Call helper n with r0 = pc (and r1 = arg if given), then leave the
 * block at the pc it returns.
 */
static int tr_helper_exit(tctx_t *t, int n, int have_arg, uint32_t arg, int arg_dreg)
{
        emit_flush_flags(t);
        if (arg_dreg >= 0) {
                t_ldr(E, R1, R4, OFF_D(arg_dreg));
                t_uxth(E, R1, R1);      /* the word is the SR; the high half is the length extra */
        }
        else if (have_arg)
                t_mov32(E, R1, arg);
        t_mov32(E, R0, t->pc);
        call_helper(t, n);
        t->f.r9kind = F_NONE;           /* flags may have changed under us */
        emit_exit_reg(t, R0, t->count + 1);
        return 1;
}

/* MOVE SR,<ea> (not privileged on the 68000) */
static int tr_move_from_sr(tctx_t *t, uint32_t op)
{
        int mode = op >> 3 & 7, r = op & 7;
        if (!ea_ok(mode, r, V_DALT))
                return -1;
        emit_flush_flags(t);
        if (mode != EA_D)
                emit_ea_addr(t, mode, r, 2, R10);
        call_helper(t, JH_GETSR);
        t_mov(E, R8, R0);
        if (mode == EA_D)
                emit_write_dreg(t, r, 2, R8);
        else
                emit_store(t, 2, R10, R8);
        return 0;
}

static int translate_one(tctx_t *t, uint32_t op)
{
        switch (op) {
        case 0x007c: return tr_helper_exit(t, JH_SROP, 1, 0 << 16 | fetch16(t), -1);
        case 0x027c: return tr_helper_exit(t, JH_SROP, 1, 1u << 16 | fetch16(t), -1);
        case 0x0a7c: return tr_helper_exit(t, JH_SROP, 1, 2u << 16 | fetch16(t), -1);
        case 0x4e73: return tr_helper_exit(t, JH_RTE, 0, 0, -1);
        case 0x46fc: return tr_helper_exit(t, JH_MOVETOSR, 1, 2u << 16 | fetch16(t), -1);
        }
        if ((op & 0xfff8) == 0x46c0)                    /* MOVE Dn,SR */
                return tr_helper_exit(t, JH_MOVETOSR, 0, 0, op & 7);
        if ((op & 0xffc0) == 0x46c0)                    /* MOVE <ea>,SR: let Musashi */
                return tr_helper_exit(t, JH_INTERP, 0, 0, -1);
        if ((op & 0xffc0) == 0x40c0)
                return tr_move_from_sr(t, op);
        switch (op >> 12) {
        case 0xa:
                return tr_aline(t);
        case 0x0:
                if (op & 0x100 || (op & 0xf00) == 0x800)
                        return tr_bitop(t, op);
                return tr_immop(t, op);
        case 0x1: case 0x2: case 0x3:
                return tr_move(t, op);
        case 0x4:
                if (op == 0x4e71)
                        return 0;                               /* NOP */
                if (op == 0x4e75)
                        return tr_rts(t);
                if ((op & 0xf1c0) == 0x41c0)
                        return tr_lea(t, op);
                if ((op & 0xffc0) == 0x4840 && (op & 0x38))
                        return tr_pea(t, op);
                if ((op & 0xfff8) == 0x4840 || (op & 0xfff8) == 0x4880 || (op & 0xfff8) == 0x48c0)
                        return tr_ext_swap(t, op);
                if ((op & 0xfb80) == 0x4880)
                        return tr_movem(t, op);
                if ((op & 0xfff8) == 0x4e50)
                        return tr_link(t, op);
                if ((op & 0xfff8) == 0x4e58)
                        return tr_unlk(t, op);
                if ((op & 0xff80) == 0x4e80)
                        return tr_jmp_jsr(t, op);
                if ((op & 0xff00) == 0x4200)
                        return tr_unary(t, op, 0);
                if ((op & 0xff00) == 0x4400)
                        return tr_unary(t, op, 1);
                if ((op & 0xff00) == 0x4600)
                        return tr_unary(t, op, 2);
                if ((op & 0xff00) == 0x4a00)
                        return tr_unary(t, op, 3);
                return -1;
        case 0x5:
                if ((op & 0xc0) == 0xc0) {
                        if ((op & 0x38) == 0x08)
                                return tr_dbcc(t, op);
                        return tr_scc(t, op);
                }
                return tr_addq_subq(t, op);
        case 0x6:
                return tr_bcc(t, op);
        case 0x7:
                return tr_moveq(t, op);
        case 0x8:
                return tr_alu(t, op, 0x8);
        case 0x9:
                return tr_alu(t, op, 0x9);
        case 0xb:
                if ((op & 0x138) == 0x108 && (op & 0xc0) != 0xc0)
                        return tr_cmpm(t, op);
                return tr_alu(t, op, 0xb);
        case 0xc:
                if ((op & 0x1c0) == 0x0c0 || (op & 0x1c0) == 0x1c0)
                        return tr_mul(t, op);
                if ((op & 0x130) == 0x100 && (op & 0x1f8) != 0x100 && (op & 0x1f8) != 0x108)
                        return tr_exg(t, op);
                return tr_alu(t, op, 0xc);
        case 0xd:
                return tr_alu(t, op, 0xd);
        case 0xe:
                return tr_shift(t, op);
        }
        return -1;
}

/* -------------------------------------------------------------------- */
/* Blocks                                                               */

static jit_entry_t *lookup_set(uint32_t pc)
{
        uint32_t h = (pc * 0x9e3779b1u) >> (32 - TABLE_BITS);
        return &table[h * 2];
}

static void recycle_code(void)
{
        if (plat->recycle)
                plat->recycle();
        code_ptr = perm_end + m68k_jit_code_skew / 2;
        flush_all();
        m68k_jit_stats.recycles++;
}

/* The distinct RAM pages covered by the trace's ranges (plus, optionally,
 * one more range); returns how many, or MAX_PAGES + 1 if too many.
 */
static int trace_pages(const tctx_t *t, uint32_t xs, uint32_t xe, uint16_t *out)
{
        int n = 0;
        for (int i = 0; i <= t->nranges + t->nxw; i++) {
                uint32_t s0 = i < t->nranges ? t->rstart[i] : i == t->nranges ? xs : t->xw[i - t->nranges - 1];
                uint32_t e0 = i < t->nranges ? t->rend[i] : i == t->nranges ? xe : s0 + 2;
                s0 &= 0xffffff;
                e0 &= 0xffffff;
                if (s0 >= ram_size || e0 <= s0)
                        continue;
                if (e0 > ram_size)
                        e0 = ram_size;
                for (uint32_t p = s0 >> PAGE_SHIFT; p <= (e0 - 1) >> PAGE_SHIFT; p++) {
                        int seen = 0;
                        for (int k = 0; k < n; k++)
                                seen |= out[k] == p;
                        if (seen)
                                continue;
                        if (n == MAX_PAGES)
                                return MAX_PAGES + 1;
                        out[n++] = p;
                }
        }
        return n;
}

/* Note which RAM words the trace was translated from, and which pages
 * (with their current generations) it depends on.
 */
static void mark_code(const tctx_t *t, jit_entry_t *ent)
{
        uint16_t pages[MAX_PAGES];
        int n = trace_pages(t, 0, 0, pages);
        for (int i = 0; i < MAX_PAGES; i++)
                ent->pg[i] = NOPAGE;
        for (int r = 0; r < t->nranges; r++) {
                uint32_t start = t->rstart[r] & 0xffffff, end = t->rend[r] & 0xffffff;
                if (start >= ram_size || end <= start)
                        continue;
                if (end > ram_size)
                        end = ram_size;
                for (uint32_t w = start >> 1; w <= (end - 1) >> 1; w++)
                        codebits[w >> 3] |= 1 << (w & 7);
        }
        for (int k = 0; k < t->nxw; k++) {
                uint32_t a = t->xw[k] & 0xffffff;
                if (a < ram_size)
                        codebits[a >> 4] |= 1 << (a >> 1 & 7);
        }
        for (int i = 0; i < n && i < MAX_PAGES; i++) {
                codepage[pages[i]] = 1;
                ent->pg[i] = pages[i];
                ent->gg[i] = page_gen[pages[i]];
        }
}

static int entry_valid(const jit_entry_t *ent, uint32_t pc)
{
        if (ent->gen != gen || ent->pc != pc)
                return 0;
        for (int i = 0; i < MAX_PAGES; i++)
                if (ent->pg[i] != NOPAGE && page_gen[ent->pg[i]] != ent->gg[i])
                        return 0;
        return 1;
}

/* Can the trace carry on at target (an unconditional jump's destination)? */
static int can_follow(tctx_t *t, uint32_t target)
{
        if (m68k_jit_no_follow || !chainable(target) || t->nranges + 1 >= MAX_RANGES)
                return 0;
        if (!m68k_jit_no_native && m68k_native_lookup(target) >= 0)
                return 0;               /* it gets a block of its own */
        uint16_t pages[MAX_PAGES + 1];
        /* the range so far, plus room for code at the target (2 pages) */
        tctx_t tmp_unused;
        (void)tmp_unused;
        t->rend[t->nranges] = t->fetch;
        t->nranges++;
        int n = trace_pages(t, target, target + 2 * (1u << PAGE_SHIFT) - 2, pages);
        t->nranges--;
        return n <= MAX_PAGES;
}

/* The translation depends on the 68k word at pc (besides its ranges);
 * returns 0 if that's more pages than a block can track.
 */
static int add_word_dep(tctx_t *t, uint32_t pc)
{
        if ((pc & 0xffffff) >= ram_size)
                return 1;               /* ROM */
        if (t->nxw >= 4)
                return 0;
        uint16_t pages[MAX_PAGES + 1];
        int n;
        if (!t->closed) {
                t->rend[t->nranges] = t->fetch;
                t->nranges++;
                n = trace_pages(t, pc, pc + 2, pages);
                t->nranges--;
        } else {
                n = trace_pages(t, pc, pc + 2, pages);
        }
        if (n > MAX_PAGES)
                return 0;
        t->xw[t->nxw++] = pc;
        return 1;
}

/* Translate the block at pc.  Returns NULL if the first instruction isn't
 * something we handle.
 */
static uint16_t *translate(uint32_t pc, jit_entry_t *ent)
{
        if (code_end - code_ptr < 8 * BLOCK_SLACK)
                recycle_code();

        static tctx_t tc;
        tctx_t *t = &tc;
        t->e.start = t->e.p = code_ptr;
        t->e.end = code_end;
        t->e.full = 0;
        t->e.defer = 0;
        t->start_pc = pc;
        t->count = 0;
        t->nstubs = 0;
        t->f.pend = F_NONE;
        t->f.xpend = 0;
        t->f.r9kind = F_NONE;
        t->f.apsr = 0;
        t->nranges = 0;
        t->closed = 0;
        t->nxw = 0;
        t->rstart[0] = pc;
        emit_prologue(t);
        uint16_t *body = t->e.p;

        uint32_t at = pc;
        int ended = 0;
        while (t->count < MAX_BLOCK_INSNS && !ended) {
                if (!fetchable(at) || code_end - t->e.p < BLOCK_SLACK)
                        break;
                uint16_t *mark = t->e.p;
                fstate_t fmark = t->f;
                int smark = t->nstubs, dmark = t->e.defer;
                int ni = m68k_jit_no_native ? -1 : m68k_native_lookup(at);
                if (ni >= 0) {
                        /* A native routine: alone in its block */
                        if (t->count)
                                break;
                        emit_native(t, ni, at);
                        at += 2;
                        t->count++;
                        ended = 1;
                        break;
                }
                t->pc = at;
                t->fetch = at + 2;
                t->fail = 0;
                uint32_t op = peek16(at);
                uint16_t *istart = t->e.p;
                t->alu_imm_valid = 0;
                if (t->e.defer) {
                        /* Flags still only in APSR, and this instruction
                         * overwrites them before anything can look: they
                         * never need capturing.
                         */
                        int k = kills_flags(op);
                        if ((k & KF_NZVC) && (!t->f.xpend || (k & KF_X))) {
                                t->e.defer = 0;
                                t->f.pend = F_NONE;
                                t->f.xpend = 0;
                                t->f.r9kind = F_NONE;
                        }
                }
#ifdef JIT_DEBUG_PC
                t_mov32(E, R12, at);    /* crash dumps show r12: the 68k pc */
#endif
                int r = translate_one(t, op);
                if (r < 0 || t->fail || t->e.full) {
                        t->e.p = mark;          /* discard partial output */
                        t->e.full = 0;
                        t->f = fmark;
                        t->nstubs = smark;
                        t->e.defer = dmark;
                        break;
                }
                at = t->fetch;
                if (m68k_jit_size_observer)
                        m68k_jit_size_observer(t->pc, op, (uint32_t)((char *)t->e.p - (char *)istart));
                t->count++;
                ended = r == 1;
                if (r == 2) {
                        /* Unconditional jump folded into the trace: close
                         * this range of 68k code, open one at the target.
                         */
                        t->rend[t->nranges++] = t->fetch;
                        t->rstart[t->nranges] = at = t->follow;
                        m68k_jit_stats.follows++;
                }
        }
        t->rend[t->nranges++] = at;
        t->closed = 1;

        if (t->count == 0 && t->e.p == body) {
                for (int i = 0; i < MAX_PAGES; i++)
                        ent->pg[i] = NOPAGE;
                return NULL;
        }
        if (!ended)
                emit_exit_const(t, at, t->count);
        for (int i = 0; i < t->nstubs; i++)
                emit_stub(t, &t->stubs[i]);
        if (t->e.full) {
                /* Ran out of buffer: start over in a fresh one */
                recycle_code();
                return translate(pc, ent);
        }

        mark_code(t, ent);
        uint16_t *code = code_ptr;
        /* (The platform's code_written evicts any stale I-cache copies of
         * these lines, including one shared with the previous block.)
         */
        code_ptr = (uint16_t *)(((uintptr_t)t->e.p + m68k_jit_code_pad + CODE_ALIGN - 1) & ~(uintptr_t)(CODE_ALIGN - 1));
        m68k_jit_stats.code_bytes += (uint32_t)((char *)t->e.p - (char *)code);
        plat->code_written(code, (uint32_t)((char *)t->e.p - (char *)code));
        if (m68k_jit_translate_observer)
                m68k_jit_translate_observer(pc, code, (uint32_t)((char *)t->e.p - (char *)code));
        m68k_jit_stats.translations++;
        return code;
}

/* Trampoline and shared flag routines, at the start of the code buffer
 * where recycling never touches them.
 */
static void emit_permanent(void)
{
        temit_t pe = { .p = code_buf, .start = code_buf, .end = code_buf + PERM_HALFWORDS };
        temit_t *e = &pe;

        /* uint32_t tramp(uint32_t block_thumb_addr) */
        entry_tramp = e->p;
        t_push(e, 0x4FF8);              /* r3-r11, lr: 10 regs, SP stays 8-aligned */
        t_mov(e, R4, R1);               /* the register file */
        t_mov32(e, R5, plat->taddr(ram));
        t_mov32(e, R6, plat->taddr(codepage));
        t_ldr(e, R7, R4, OFF_BUDGET);
        t_blx(e, R0);
        t_str(e, R7, R4, OFF_BUDGET);
        t_pop(e, 0x8FF8);               /* r3-r11, pc */

        /* Flag materialisers: r9 -> n/not_z/v/c (+x).  Only non-flag-setting
         * instructions, so APSR survives (Bcc relies on it); clobber r2, r3.
         */
        for (int kind = F_ADD; kind <= F_SHIFT; kind++) {
                for (int x = 0; x < 2; x++) {
                        if ((uintptr_t)e->p & 2)
                                t_nop(e);
                        mat_fn[kind][x] = e->p;
                        t_movsh(e, 0, R2, R9, SH_LSR, 24);
                        t_andi(e, R2, R2, 0x80);
                        t_str(e, R2, R4, OFF_N);
                        t_andi(e, R2, R9, 0x40000000);
                        t_eori(e, R2, R2, 0x40000000);
                        t_str(e, R2, R4, OFF_Z);
                        if (kind == F_LOGIC) {
                                t_mov32(e, R2, 0);
                                t_str(e, R2, R4, OFF_V);
                                t_str(e, R2, R4, OFF_C);
                        } else {
                                t_movsh(e, 0, R3, R9, SH_LSR, 21);      /* bit 7 = V, bit 8 = C */
                                if (kind == F_SHIFT)
                                        t_mov32(e, R2, 0);
                                else
                                        t_andi(e, R2, R3, 0x80);
                                t_str(e, R2, R4, OFF_V);
                                t_andi(e, R3, R3, 0x100);
                                if (kind == F_SUB)
                                        t_eori(e, R3, R3, 0x100);       /* ARM C = !borrow */
                                t_str(e, R3, R4, OFF_C);
                        }
                        if (x && kind != F_LOGIC) {
                                t_str(e, R3, R4, OFF_X);
                        }
                        t_bx(e, LR);
                }
        }
        /* X only, from r9 as left by an op of `kind`: r3 */
        for (int kind = F_ADD; kind <= F_SHIFT; kind++) {
                if (kind == F_LOGIC)
                        continue;
                matx_fn[kind] = e->p;
                t_movsh(e, 0, R3, R9, SH_LSR, 21);
                t_andi(e, R3, R3, 0x100);
                if (kind == F_SUB)
                        t_eori(e, R3, R3, 0x100);
                t_str(e, R3, R4, OFF_X);
                t_bx(e, LR);
        }
        /* Memory access.  rdN: r0 = addr -> r0 = value.  wrN: r0 = addr,
         * r1 = value.  RAM fast path first; ROM next (reads); else C.
         * Clobber r0-r3, r12, lr.
         */
        for (int size = 1; size <= 4; size <<= 1) {
                int hn = size == 1 ? 0 : size == 2 ? 1 : 2;
                if ((uintptr_t)e->p & 2)
                        t_nop(e);
                rd_fn[size] = e->p;
                t_bici(e, R1, R0, 0xff000000);
                t_tsti(e, R1, 0xc00000);
                tbr_t not_ram = t_b_placeholder(e, C_NE);
                if (size == 1) {
                        t_ldstr(e, LS_LDRB, R0, R5, R1);
                } else if (size == 2) {
                        t_ldstr(e, LS_LDRH, R0, R5, R1);
                        t_rev16(e, R0, R0);
                } else {
                        t_ldstr(e, LS_LDR, R0, R5, R1);
                        t_rev(e, R0, R0);
                }
                t_bx(e, LR);
                t_patch_branch(not_ram, e->p);
                t_andi(e, R2, R1, 0xf00000);
                t_cmpi(e, R2, 0x400000);
                tbr_t not_rom = t_b_placeholder(e, C_NE);
                t_ubfx(e, R1, R1, 0, 31 - __builtin_clz(rom_size));
                t_mov32(e, R3, plat->taddr(rom));
                if (size == 1) {
                        t_ldstr(e, LS_LDRB, R0, R3, R1);
                } else if (size == 2) {
                        t_ldstr(e, LS_LDRH, R0, R3, R1);
                        t_rev16(e, R0, R0);
                } else {
                        t_ldstr(e, LS_LDR, R0, R3, R1);
                        t_rev(e, R0, R0);
                }
                t_bx(e, LR);
                t_patch_branch(not_rom, e->p);
                t16(e, 0xB508);                         /* push {r3, lr} */
                t_mov32(e, R12, plat->helper_addr(JH_RD8 + hn));
                t_blx(e, R12);
                t16(e, 0xBD08);                         /* pop {r3, pc} */

                if ((uintptr_t)e->p & 2)
                        t_nop(e);
                wr_fn[size] = e->p;
                t_bici(e, R2, R0, 0xff000000);
                t_tsti(e, R2, 0xc00000);
                tbr_t slow = t_b_placeholder(e, C_NE);
                if (size == 1) {
                        t_ldstr(e, LS_STRB, R1, R5, R2);
                } else if (size == 2) {
                        t_rev16(e, R3, R1);
                        t_ldstr(e, LS_STRH, R3, R5, R2);
                } else {
                        t_rev(e, R3, R1);
                        t_ldstr(e, LS_STR, R3, R5, R2);
                }
                /* Did we just write over translated code? */
                t_movsh(e, 0, R3, R2, SH_LSR, PAGE_SHIFT);
                t_ldstr(e, LS_LDRB, R3, R6, R3);
                t_cmpi(e, R3, 0);
                tbr_t cw = t_b_placeholder(e, C_NE);
                t_movs8(e, R0, 0);                      /* no code overwritten */
                t_bx(e, LR);
                t_patch_branch(cw, e->p);
                t16(e, 0xB508);
                t_mov(e, R0, R2);
                t_mov32(e, R1, size);
                t_mov32(e, R12, plat->helper_addr(JH_CODEWRITE));
                t_blx(e, R12);
                t16(e, 0xBD08);
                t_patch_branch(slow, e->p);
                t16(e, 0xB508);
                t_mov32(e, R12, plat->helper_addr(JH_WR8 + hn));
                t_blx(e, R12);                          /* returns 0 */
                t16(e, 0xBD08);
        }
        /* Helper veneers, so generated code can reach C with a 4-byte BL */
        for (int n = 0; n < JH_COUNT; n++) {
                if ((uintptr_t)e->p & 2)
                        t_nop(e);
                helper_veneer[n] = e->p;
                t_mov32(e, R12, plat->helper_addr(n));
                t_bx(e, R12);
        }

        /* dyn_jump: branched to (not called) from a block exit, with
         * REG_PC set and budget left.  Hit: jump into the target block's
         * body.  Miss: back to the dispatcher, which fills the entry.
         */
        if ((uintptr_t)e->p & 2)
                t_nop(e);
        dyn_jump = e->p;
        t_ldr(e, R0, R4, OFF_PC);
        t_mov32(e, R1, 0x9e3779b1u);
        t_mul(e, R1, R0, R1);
        t_movsh(e, 0, R1, R1, SH_LSR, 32 - JC_BITS);
        t_mov32(e, R2, plat->taddr(jcache));
        t_dpr(e, DP_ADD, 0, R1, R2, R1, SH_LSL, 3);
        t_ldr(e, R2, R1, 0);
        t_cmp(e, R2, R0);
        tbr_t miss = t_b_placeholder(e, C_NE);
        t_ldr(e, R3, R1, 4);
        t_bx(e, R3);
        t_patch_branch(miss, e->p);
        t_mov32(e, R2, JC_MISS);
        t_str(e, R2, R4, OFF_LASTSLOT);
        t16(e, 0xBD08);                         /* pop {r3, pc} */

        /* exit_common: a chainable exit's stub branches here with r0 =
         * its record's index + 1, for the dispatcher to link it.
         */
        if ((uintptr_t)e->p & 2)
                t_nop(e);
        exit_common = e->p;
        t_str(e, R0, R4, OFF_LASTSLOT);
        t_mov32(e, R1, plat->taddr(exit_pc) - 4);
        t32(e, 0xF850 | R1, R1 << 12 | 2 << 4 | R0);  /* ldr.w r1, [r1, r0, lsl #2] */
        t_str(e, R1, R4, OFF_PC);
        t16(e, 0xBD08);                         /* pop {r3, pc} */

        /* Fused addressing-mode entry points, falling into rd_fn/wr_fn */
        for (int mode = 0; mode < 3; mode++) {
                for (int an = 0; an < 8; an++) {
                        for (int size = 1; size <= 4; size <<= 1) {
                                int step = (size == 1 && an == 7) ? 2 : size;
                                for (int wr = 0; wr < 2; wr++) {
                                        uint16_t *f = e->p;
                                        t_ldr(e, R0, R4, OFF_A(an));
                                        if (mode == 1) {
                                                t_addi(e, R2, R0, step);
                                                t_str(e, R2, R4, OFF_A(an));
                                        } else if (mode == 2) {
                                                t_subi(e, R0, R0, step);
                                                t_str(e, R0, R4, OFF_A(an));
                                        }
                                        t_b_to(e, C_AL, wr ? wr_fn[size] : rd_fn[size]);
                                        if (wr)
                                                ea_wr[mode][an][size] = f;
                                        else
                                                ea_rd[mode][an][size] = f;
                                }
                        }
                }
        }

        /* rts_fn: branched to from a block (budget already charged, flags
         * stored): pop the return address and carry on like a computed exit.
         */
        if ((uintptr_t)e->p & 2)
                t_nop(e);
        rts_fn = e->p;
        t_ldr(e, R8, R4, OFF_A(7));
        t_mov(e, R0, R8);
        t_bl_to(e, rd_fn[4]);
        t_addi(e, R8, R8, 4);
        t_str(e, R8, R4, OFF_A(7));
        t_str(e, R0, R4, OFF_PC);
        t_cmpi(e, R7, 0);
        tbr_t out = t_b_placeholder(e, C_LE);
        t_b_to(e, C_AL, dyn_jump);
        t_patch_branch(out, e->p);
        t16(e, 0xBD08);                         /* pop {r3, pc} */

        perm_end = (uint16_t *)(((uintptr_t)e->p + 31) & ~(uintptr_t)31);
        plat->code_written(code_buf, (uint32_t)((char *)e->p - (char *)code_buf));
        code_ptr = perm_end + m68k_jit_code_skew / 2;
}

int m68k_jit_init(const m68kjit_platform_t *p, uint8_t *r, uint32_t rsize,
                  const uint8_t *ro, uint32_t rosize, uint32_t code_size)
{
        plat = p;
        ram = r;
        ram_size = rsize;
        rom = ro;
        rom_size = rosize;
        npages = rsize >> PAGE_SHIFT;
        codepage = plat->alloc(npages);
        table = plat->alloc(sizeof(jit_entry_t) << (TABLE_BITS + 1));
        code_buf = plat->alloc(code_size);
        codebits = plat->alloc(rsize / 16);
        page_gen = plat->alloc(npages * sizeof *page_gen);
        exits = plat->alloc(MAX_EXITS * sizeof *exits);
        exit_pc = plat->alloc(MAX_EXITS * sizeof *exit_pc);
        links = plat->alloc(MAX_LINKS * sizeof *links);
        page_links = plat->alloc(npages * sizeof *page_links);
        if (!codepage || !table || !code_buf || !codebits || !page_gen || !exits || !exit_pc || !links || !page_links)
                return -1;
        for (uint32_t p = 0; p < npages; p++)
                page_links[p] = -1;
        jcache = plat->alloc(sizeof(jc_t) << JC_BITS);
        if (!jcache)
                return -1;
        jcache_clear();
        memset(codepage, 0, npages);
        memset(codebits, 0, rsize / 16);
        memset(page_gen, 0, npages * sizeof *page_gen);
        memset(table, 0, sizeof(jit_entry_t) << (TABLE_BITS + 1));
        code_end = code_buf + code_size / 2;
        emit_permanent();
        return 0;
}

/* Find (or translate) the block for pc */
static jit_entry_t *get_block(uint32_t pc)
{
        jit_entry_t *set = lookup_set(pc), *ent;
        if (entry_valid(&set[0], pc))
                return &set[0];
        if (entry_valid(&set[1], pc))
                return &set[1];
        /* Prefer an empty/stale way; else alternate */
        if (set[0].gen != gen || set[0].pc == pc)
                ent = &set[0];
        else if (set[1].gen != gen || set[1].pc == pc)
                ent = &set[1];
        else
                ent = &set[m68k_jit_stats.translations & 1];
        if (ent->gen == gen && ent->pc != pc)
                m68k_jit_stats.conflicts++;
        else if (ent->gen == gen)
                m68k_jit_stats.stale++;
        float t0 = plat->now ? plat->now() : 0;
        ent->code = translate(pc, ent);
        if (plat->now)
                m68k_jit_stats.t_xlat += plat->now() - t0;
        ent->pc = pc;
        ent->gen = gen;
        return ent;
}

/* Exit record e just returned to the dispatcher with REG_PC set: point
 * its slot straight at the block for REG_PC.
 */
static void link_exit(uint32_t e, uint32_t recycles)
{
        jit_entry_t *next = get_block(J->pc);
        if (!next->code || m68k_jit_stats.recycles != recycles || e >= nexits)
                return;
        /* An exit that leaves its flags unstored needs a target that
         * overwrites them (RAM code may have changed since it was built)
         */
        uint32_t need = exits[e].stub >> EXIT_NEED_SHIFT;
        if (need && (target_kills_flags(J->pc) & need) != need)
                return;
        /* RAM targets: remember the link so retiring the page undoes it */
        for (int i = 0; i < MAX_PAGES; i++) {
                uint16_t pg = next->pg[i];
                if (pg == NOPAGE)
                        continue;
                if (nlinks >= MAX_LINKS)
                        return;
                links[nlinks].exit_hi = e >> 16;
                links[nlinks].exit_lo = e & 0xffff;
                links[nlinks].next = page_links[pg];
                page_links[pg] = nlinks++;
        }
        uint16_t *slot = (uint16_t *)((char *)code_buf + exits[e].slot);
        tbr_t b = { slot, C_AL };
        t_patch_branch(b, next->code + 1);      /* past its push */
        plat->code_written(slot, 4);
        m68k_jit_stats.chains++;
}

/* Musashi's interrupt check, only when there's something to take */
static void check_interrupts(void)
{
        if (m68ki_cpu.nmi_pending || CPU_INT_LEVEL > FLAG_INT_MASK) {
                m68k_jit_sync_out();
                m68ki_check_interrupts();
                m68k_jit_sync_in();
        }
}

void (*m68k_jit_interp_observer)(uint32_t pc, uint32_t opcode);

/* One instruction through Musashi */
static void interp_one(void)
{
        if (m68k_jit_interp_observer)
                m68k_jit_interp_observer(J->pc, peek16(J->pc));
        float t0 = plat->now ? plat->now() : 0;
        m68k_jit_sync_out();
        m68k_step_one();
        m68k_jit_sync_in();
        m68k_jit_stats.interp_instrs++;
        if (plat->now)
                m68k_jit_stats.t_interp += plat->now() - t0;
}

/* Re-layout.  Translations land in the code buffer in the order they're
 * first needed, so once the machine settles into a loop (the Finder
 * idling, an application waiting for input), the code it runs is
 * scattered among everything translated before.  Starting over then
 * retranslates just that working set, in the order it runs: chained
 * blocks end up next to each other and nothing dead shares their cache
 * lines.  So: after a quiet stretch (hardly any new translations) that
 * follows a lot of new code, throw the translations away once.
 */
#define RELAYOUT_WINDOW 1000000         /* 68k instructions per check (~1s) */
#define RELAYOUT_QUIET  4               /* at most this many translations in it */
#define RELAYOUT_BYTES  (256 * 1024)    /* code generated since the last start-over */
int m68k_jit_relayout = 1;             /* 0: off; n: after n * RELAYOUT_BYTES of new code */

static void maybe_relayout(void)
{
        static uint64_t win_instrs;
        static uint32_t win_xlat;
        uint64_t done = m68k_jit_stats.jit_instrs + m68k_jit_stats.interp_instrs;
        if (done - win_instrs < RELAYOUT_WINDOW)
                return;
        int quiet = m68k_jit_stats.translations - win_xlat <= RELAYOUT_QUIET;
        win_instrs = done;
        win_xlat = m68k_jit_stats.translations;
        if (m68k_jit_relayout && quiet && (uint32_t)((char *)code_ptr - (char *)perm_end) >= (uint32_t)m68k_jit_relayout * RELAYOUT_BYTES) {
                recycle_code();
                m68k_jit_stats.relayouts++;
        }
}

int m68k_jit_execute(int num_cycles)
{
        if (!plat)
                return m68k_execute(num_cycles);        /* JIT unavailable */
        maybe_relayout();
        float t_start = plat->now ? plat->now() : 0;
        jregs_t local;
        J = plat->regs ? plat->regs() : &local;
        m68k_jit_sync_in();
        m68ki_remaining_cycles = num_cycles;
        if (sleeping) {
                /* Idle: sleep on until an interrupt is taken */
                uint32_t pc0 = J->pc;
                check_interrupts();
                if (J->pc == pc0 || CPU_STOPPED) {
                        m68k_jit_sync_out();
                        m68k_jit_stats.idle_quanta++;
                        return num_cycles;
                }
                sleeping = 0;
        }
        check_interrupts();

        while (m68ki_remaining_cycles > 0) {
                if (CPU_STOPPED) {
                        m68ki_remaining_cycles = 0;
                        break;
                }
                if (!m68k_jit_enabled || overlay) {
                        interp_one();
                        check_interrupts();
                        continue;
                }

                uint32_t pc = J->pc;
                jit_entry_t *ent = get_block(pc);
                if (ent->code) {
                        int32_t budget = (m68ki_remaining_cycles + 7) / 8;
                        if (m68k_jit_max_budget && budget > m68k_jit_max_budget)
                                budget = m68k_jit_max_budget;
                        J->budget = budget;
                        J->lastexit = 0;
                        uint32_t recycles = m68k_jit_stats.recycles;
                        float t0 = plat->now ? plat->now() : 0;
                        plat->run(entry_tramp, ent->code, J);
                        if (plat->now)
                                m68k_jit_stats.t_run += plat->now() - t0;
                        int32_t n = budget - J->budget;
                        if (m68k_jit_idle_request) {
                                m68k_jit_idle_request = 0;
                                n -= IDLE_DRAIN;
                                sleeping = 1;
                                m68ki_remaining_cycles = 0;
                        }
                        m68ki_remaining_cycles -= 8 * n;
                        m68k_jit_stats.blocks++;
                        m68k_jit_stats.jit_instrs += n;

                        /* Took a chainable exit: link it to its target */
                        uint32_t ex = J->lastexit;
                        if (ex == JC_MISS) {
                                J->lastexit = 0;
                                jit_entry_t *next = get_block(J->pc);
                                if (next->code && m68k_jit_stats.recycles == recycles && !m68k_jit_no_jcache) {
                                        jc_t *jc = &jcache[jc_hash(J->pc)];
                                        jc->pc = J->pc;
                                        jc->code = plat->taddr(next->code + 1) | 1;
                                }
                        } else if (ex) {
                                J->lastexit = 0;
                                link_exit(ex - 1, recycles);
                        }
                } else {
                        interp_one();
                }
                check_interrupts();
        }
        m68k_jit_sync_out();
        if (plat->now)
                m68k_jit_stats.t_total += plat->now() - t_start;
        return num_cycles - m68ki_remaining_cycles;
}

/* Throw away all translations (between m68k_jit_execute calls) */
void m68k_jit_flush_code(void)
{
        if (plat)
                recycle_code();
}

/* Debugging: translate the block at pc and return its code */
uint16_t *m68k_jit_debug_translate(uint32_t pc, uint32_t *len)
{
        jit_entry_t tmp;
        uint16_t *before = code_ptr;
        uint16_t *code = translate(pc, &tmp);
        *len = code ? (uint32_t)((char *)code_ptr - (char *)before) : 0;
        return code;
}
