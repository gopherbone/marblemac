/* Lockstep verifier for the 68k JIT.
 *
 * Boots the Mac with the JIT enabled; translated Thumb-2 blocks run under
 * Unicorn (Cortex-M7).  After each block, its RAM writes are undone, the
 * CPU state restored, and Musashi re-executes the same instructions; the
 * two results (registers, flags, memory) must match.  Musashi's result is
 * kept, so one bad block doesn't derail the rest of the run.
 *
 * usage: jit_verify rom.bin disk.img outprefix [script...]
 *   script: "m X Y" glide mouse, "c" click, "dc" double-click,
 *           "w MS" wait, "s NAME" snapshot, "t SECS" run for SECS
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unicorn/unicorn.h>

#include "umac.h"
#include "machw.h"
#include "rom.h"
#include "m68k.h"
#include "m68kjit.h"
#include "macglue.h"

/* Musashi internals we compare */
#include "m68kcpu.h"

#define T_RAM   0x10000000u
#define T_ROM   0x18000000u
#define T_CPU   0x20000000u
#define T_MAP   0x21000000u
#define T_CODE  0x30000000u
#define T_HELP  0x0f000000u    /* Cortex-M: must be an executable region */
#define T_STACK 0x2f000000u
#define T_RET   0x0e000000u
#define T_REGS  0x22000000u

#define CODE_SIZE (2u << 20)

static uint32_t ring[64];
static int ringi;
static uint32_t *iexec;                 /* executions per 68k pc (from Musashi replay PCs) */

/* ---- Playdate (Rev B) cache model, for estimating device speed ---- */
typedef struct {
        int ways, sets;
        uint32_t *tag;          /* [sets * ways], 0 = empty */
        uint32_t *age;
        uint32_t clock;
        uint64_t hits, misses, seq_misses;
        uint32_t last_line;
} cache_t;

static cache_t icache, dcache;
static uint64_t sim_arm_instrs, sim_stores;

static void cache_init(cache_t *c, int size, int ways)
{
        c->ways = ways;
        c->sets = size / 32 / ways;
        c->tag = calloc(c->sets * ways, 4);
        c->age = calloc(c->sets * ways, 4);
}

/* Returns 0 hit, 1 sequential miss (line after the last miss), 2 random miss */
static int cache_access(cache_t *c, uint32_t addr, int allocate)
{
        uint32_t line = addr >> 5, set = line % c->sets, tag = line | 0x80000000u;
        uint32_t *t = c->tag + set * c->ways, *a = c->age + set * c->ways;
        c->clock++;
        for (int w = 0; w < c->ways; w++)
                if (t[w] == tag) {
                        a[w] = c->clock;
                        c->hits++;
                        return 0;
                }
        int seq = line == c->last_line + 1;
        c->last_line = line;
        if (seq)
                c->seq_misses++;
        else
                c->misses++;
        if (allocate) {
                int victim = 0;
                for (int w = 1; w < c->ways; w++)
                        if (a[w] < a[victim])
                                victim = w;
                t[victim] = tag;
                a[victim] = c->clock;
        }
        return seq ? 1 : 2;
}

static uint64_t imiss_from_perm_linestart;
static uint64_t imiss_perm, imiss_entry, imiss_mid, imiss_from_perm, imiss_back, imiss_fwd;
static uint32_t sim_prev_addr;
/* OPHIST: executed instructions in translated blocks, by kind */
static int ophist_on;
static const char *ophist_name[] = { "mrs", "msr", "bl", "ldr/str [r4] (regfile)", "ldr/str other",
        "mov/movw/movt", "b/bcc/cbz", "subs r7 (budget)", "push/pop", "dp/other 16", "dp/other 32" };
static uint64_t ophist[11][2];
static uint64_t ophist_reload, ophist_reload_any;
static void ophist_count(uc_engine *u, uint32_t a, uint32_t size)
{
        uint16_t h[2] = { 0, 0 };
        uc_mem_read(u, a, h, size > 4 ? 4 : size);
        int k;
        uint16_t h1 = h[0], h2 = h[1];
        if (size == 2) {
                if ((h1 & 0xFF00) == 0x3F00) k = 7;
                else if (h1 >= 0x5000 && h1 < 0xA000) k = ((h1 >> 3 & 7) == 4 && h1 >= 0x6000) ? 3 : 4;
                else if ((h1 & 0xFF00) == 0x4600 || (h1 & 0xF800) == 0x2000) k = 5;
                else if ((h1 & 0xF000) == 0xD000 || (h1 & 0xF800) == 0xE000 || (h1 & 0xF500) == 0xB100) k = 6;
                else if (h1 == 0xB508 || h1 == 0xBD08) k = 8;
                else k = 9;
        } else {
                if (h1 == 0xF3EF) k = 0;
                else if ((h1 & 0xFFF0) == 0xF380 && (h2 & 0xFF00) == 0x8800) k = 1;
                else if ((h1 & 0xF800) == 0xF000 && (h2 & 0xD000) == 0xD000) k = 2;
                else if ((h1 & 0xF800) == 0xF000 && (h2 & 0x8000)) k = 6;
                else if ((h1 & 0xFE00) == 0xF800) k = (h1 & 0xF) == 4 ? 3 : 4;
                else if ((h1 & 0xFB40) == 0xF240 || ((h1 & 0xFBEF) == 0xF04F && !(h2 & 0x8000)) || (h1 & 0xFFEF) == 0xEA4F) k = 5;
                else if ((h1 & 0xFE00) == 0xE800) k = 8;
                else k = 10;
        }
        ophist[k][0]++;
        ophist[k][1] += size;
        /* ldr r,[r4,#n] right after str r,[r4,#n] (16-bit forms only) */
        static uint16_t prev;
        if (size == 2 && (h1 & 0xF800) == 0x6800 && (prev & 0xF800) == 0x6000 && (h1 & 0x7FF) == (prev & 0x7FF))
                ophist_reload++;
        if (size == 2 && (h1 & 0xF800) == 0x6800 && (prev & 0xF800) == 0x6000 && (h1 & 0x7F8) == (prev & 0x7F8))
                ophist_reload_any++;
        prev = size == 2 ? h1 : 0;
}
static void ophist_report(void)
{
        uint64_t n = 0, b = 0;
        for (int k = 0; k < 11; k++) n += ophist[k][0], b += ophist[k][1];
        for (int k = 0; k < 11; k++)
                fprintf(stderr, "  %-26s %5.1f%% of instrs %5.1f%% of bytes\n", ophist_name[k],
                        100.0 * ophist[k][0] / n, 100.0 * ophist[k][1] / b);
        fprintf(stderr, "  reload of a just-stored slot: %llu same reg, %llu any reg\n",
                (unsigned long long)ophist_reload, (unsigned long long)ophist_reload_any);
        ophist_reload = ophist_reload_any = 0;
        memset(ophist, 0, sizeof ophist);
}
static void sim_fetch(uc_engine *u, uint64_t address, uint32_t size, void *ud)
{
        (void)u; (void)ud;
        sim_arm_instrs++;
        uint32_t a = (uint32_t)address;
        if (cache_access(&icache, a, 1) == 2) {
                /* Random miss: classify by where we jumped to */
                if (a < T_CODE + 0x4000)
                        imiss_perm++;
                else if (a != sim_prev_addr + 2 && a != sim_prev_addr + 4) {
                        imiss_entry++;          /* arrived by a jump */
                        if (sim_prev_addr < T_CODE + 0x4000) {
                                imiss_from_perm++;
                                imiss_from_perm_linestart += (a & 31) < 4;
                        }      /* returning from (or chained by) a shared routine */
                        else if (a < sim_prev_addr && sim_prev_addr - a < 4096)
                                imiss_back++;
                        else if (a > sim_prev_addr && a - sim_prev_addr < 4096)
                                imiss_fwd++;
                }
                else
                        imiss_mid++;            /* fell through into a cold line */
        }
        sim_prev_addr = a;
        if (ophist_on && a >= T_CODE)
                ophist_count(u, a, size);
}

static void sim_data(uc_engine *u, uc_mem_type type, uint64_t address, int size, int64_t value, void *ud)
{
        (void)u; (void)size; (void)value; (void)ud;
        uint32_t a = (uint32_t)address;
        if (a >= T_REGS && a < T_REGS + 0x4000)
                return;                         /* DTCM on the device */
        if (type == UC_MEM_WRITE) {
                sim_stores++;
                cache_access(&dcache, a, 0);    /* no write-allocate */
        } else {
                cache_access(&dcache, a, 1);
        }
}

static uint64_t helper_calls[JH_COUNT];
static void sim_report(const char *when)
{
        /* Measured on the device: random PSRAM miss ~1.5us; streaming the
         * next line ~0.15us; stores to PSRAM ~0.07us; ~180MHz core.
         */
        double t = sim_arm_instrs * 1.2 / 180e6 +
                   (icache.misses + dcache.misses) * 1.5e-6 +
                   (icache.seq_misses + dcache.seq_misses) * 0.15e-6 +
                   sim_stores * 0.07e-6;
        fprintf(stderr, "[%s] model: %.0fM ARM instrs, I-miss %llu (+%llu seq), D-miss %llu (+%llu seq), stores %llu"
                " => %.0f ms of device time\n", when, sim_arm_instrs / 1e6,
                (unsigned long long)icache.misses, (unsigned long long)icache.seq_misses,
                (unsigned long long)dcache.misses, (unsigned long long)dcache.seq_misses,
                (unsigned long long)sim_stores, t * 1000);
        fprintf(stderr, "    random I-misses: %llu into shared routines, %llu at jump targets, %llu falling through\n",
                (unsigned long long)imiss_perm, (unsigned long long)imiss_entry, (unsigned long long)imiss_mid);
        fprintf(stderr, "    jump-target misses: %llu from shared routines (%llu at a line start), %llu short back, %llu short forward, rest far\n",
                (unsigned long long)imiss_from_perm, (unsigned long long)imiss_from_perm_linestart, (unsigned long long)imiss_back, (unsigned long long)imiss_fwd);
        imiss_perm = imiss_entry = imiss_mid = imiss_from_perm_linestart = imiss_from_perm = imiss_back = imiss_fwd = 0;
        if (ophist_on) {
                ophist_report();
                fprintf(stderr, "  helper calls:");
                for (int i = 0; i < JH_COUNT; i++)
                        fprintf(stderr, " %llu", (unsigned long long)helper_calls[i]);
                fprintf(stderr, "\n");
        }
        memset(helper_calls, 0, sizeof helper_calls);
        sim_arm_instrs = sim_stores = 0;
        icache.misses = icache.seq_misses = icache.hits = 0;
        dcache.misses = dcache.seq_misses = dcache.hits = 0;
}

static uc_engine *uc;
static uint8_t *ram, *rom;
static uintptr_t cpu_page;
static size_t cpu_page_len;
static void *allocs[32];
static size_t alloc_len[32];
static uint32_t alloc_taddr[32];        /* 0 = not mapped yet */
static uint32_t next_data_taddr = 0x23000000u;
static int nallocs;
static void *codepage_ptr, *code_ptr;
static jregs_t *jregs_buf;              /* page-aligned, mapped at T_REGS */
static jregs_t *plat_regs(void) { return jregs_buf; }

static uint64_t verified, unverified, mismatches;
static int max_report = 20;
static int io_touched;

static void die(const char *what, uc_err err)
{
        fprintf(stderr, "%s: %s\n", what, uc_strerror(err));
        exit(1);
}

static void *aligned(size_t n)
{
        void *p;
        n = (n + 0x3fff) & ~(size_t)0x3fff;
        if (posix_memalign(&p, 0x4000, n))
                return NULL;
        memset(p, 0, n);
        return p;
}

static void *plat_alloc(size_t n)
{
        void *p = aligned(n);
        allocs[nallocs] = p;
        alloc_len[nallocs++] = n;
        return p;
}

static uint32_t plat_taddr(const void *p)
{
        uintptr_t a = (uintptr_t)p;
        if (a >= (uintptr_t)ram && a < (uintptr_t)ram + RAM_SIZE)
                return T_RAM + (uint32_t)(a - (uintptr_t)ram);
        if (a >= (uintptr_t)rom && a < (uintptr_t)rom + ROM_SIZE)
                return T_ROM + (uint32_t)(a - (uintptr_t)rom);
        if (jregs_buf && a >= (uintptr_t)jregs_buf && a < (uintptr_t)jregs_buf + 0x1000)
                return T_REGS + (uint32_t)(a - (uintptr_t)jregs_buf);
        if (a >= cpu_page && a < cpu_page + cpu_page_len)
                return T_CPU + (uint32_t)(a - cpu_page);
        /* m68k_jit_init allocates: codepage map, table, code buffer, ... */
        if (nallocs > 0 && a >= (uintptr_t)allocs[0] && a < (uintptr_t)allocs[0] + alloc_len[0])
                return T_MAP + (uint32_t)(a - (uintptr_t)allocs[0]);
        if (nallocs > 2 && a >= (uintptr_t)allocs[2] && a < (uintptr_t)allocs[2] + alloc_len[2])
                return T_CODE + (uint32_t)(a - (uintptr_t)allocs[2]);
        /* Any other JIT allocation the generated code refers to: map it on demand */
        for (int i = 3; i < nallocs; i++) {
                if (a >= (uintptr_t)allocs[i] && a < (uintptr_t)allocs[i] + alloc_len[i]) {
                        if (!alloc_taddr[i]) {
                                alloc_taddr[i] = next_data_taddr;
                                next_data_taddr += (uint32_t)((alloc_len[i] + 0xffff) & ~(size_t)0xffff);
                                uc_err err = uc_mem_map_ptr(uc, alloc_taddr[i], alloc_len[i], UC_PROT_ALL, allocs[i]);
                                if (err)
                                        die("map alloc", err);
                        }
                        return alloc_taddr[i] + (uint32_t)(a - (uintptr_t)allocs[i]);
                }
        }
        fprintf(stderr, "taddr: unmapped host pointer %p\n", p);
        exit(1);
}

static uint32_t plat_helper_addr(int n)
{
        return T_HELP + n * 4 + 1;
}

static void plat_code_written(void *p, uint32_t len)
{
        uint32_t a = plat_taddr(p);
        uc_ctl_remove_cache(uc, a, a + len);
}

static void plat_recycle(void)
{
        uc_ctl_remove_cache(uc, T_CODE, T_CODE + CODE_SIZE);
}

/* ---- write logs ---- */

typedef struct { uint32_t addr; uint8_t old, new_, from_helper; } wbyte_t;
static wbyte_t jit_w[65536];
static int njit_w;
static struct { uint32_t addr; uint8_t old[4]; } mus_w[65536];
static int nmus_w;

static void hook_mem_write(uc_engine *u, uc_mem_type type, uint64_t address, int size, int64_t value, void *ud)
{
        (void)u; (void)type; (void)ud;
        uint32_t off = (uint32_t)(address - T_RAM);
        for (int i = 0; i < size && njit_w < 65536; i++) {
                jit_w[njit_w].addr = off + i;
                jit_w[njit_w].old = ram[off + i];
                jit_w[njit_w].new_ = (uint8_t)(value >> (8 * i));   /* little-endian store */
                jit_w[njit_w].from_helper = 0;
                njit_w++;
        }
}

static int recording_mus, recording_jit;
static void observe_write(uint32_t addr, uint32_t size)
{
        /* Writes made by helpers (C code) during the JIT run: log the old
         * bytes now, fill in the new ones after the run.
         */
        if (recording_jit) {
                for (uint32_t i = 0; i < size && njit_w < 65536; i++) {
                        jit_w[njit_w].addr = addr + i;
                        jit_w[njit_w].old = ram[addr + i];
                        jit_w[njit_w].new_ = 0;
                        jit_w[njit_w].from_helper = 1;
                        njit_w++;
                }
                return;
        }
        if (!recording_mus || nmus_w >= 65536)
                return;
        mus_w[nmus_w].addr = addr;
        for (int i = 0; i < 4; i++)
                mus_w[nmus_w].old[i] = addr + i < RAM_SIZE ? ram[addr + i] : 0;
        nmus_w++;
}


static void hook_helper(uc_engine *u, uint64_t address, uint32_t size, void *ud)
{
        (void)size; (void)ud;
        int n = (int)((address - T_HELP) / 4);
        helper_calls[n]++;
        uint32_t r0, r1;
        uc_reg_read(u, UC_ARM_REG_R0, &r0);
        uc_reg_read(u, UC_ARM_REG_R1, &r1);
        if (n == JH_RD16 || n == JH_RD32 || n == JH_WR16 || n == JH_WR32)
                io_touched = 1;
        if (n == JH_MOVEM || n == JH_INTERP)
                io_touched = io_touched;        /* (their RAM writes are logged; byte I/O replays) */
        uint32_t r = m68k_jit_helper(n, r0, r1);
        uc_reg_write(u, UC_ARM_REG_R0, &r);
}

static void hook_trace(uc_engine *u, uint64_t address, uint32_t size, void *ud)
{
        (void)ud;
        uint32_t r[4], lr, sp, xpsr;
        uc_reg_read(u, UC_ARM_REG_R0, &r[0]);
        uc_reg_read(u, UC_ARM_REG_R1, &r[1]);
        uc_reg_read(u, UC_ARM_REG_LR, &lr);
        uc_reg_read(u, UC_ARM_REG_SP, &sp);
        uc_reg_read(u, UC_ARM_REG_XPSR, &xpsr);
        uint8_t b[4] = {0};
        uc_mem_read(u, address, b, size);
        fprintf(stderr, "  T %08llx [%d] %02x%02x%02x%02x r0=%08x r1=%08x lr=%08x sp=%08x xpsr=%08x\n",
                (unsigned long long)address, size, b[1], b[0], b[3], b[2], r[0], r[1], lr, sp, xpsr);
}

/* Working set: which 32-byte lines of generated code run, per window */
static uint8_t wsbits[(CODE_SIZE / 32 + 7) / 8];
static uint64_t ws_jumps, ws_instrs;
static uint32_t ws_lastline = ~0u;
static void hook_ws(uc_engine *u, uint64_t address, uint32_t size, void *ud)
{
        (void)u; (void)size; (void)ud;
        uint32_t line = (uint32_t)(address - T_CODE) / 32;
        wsbits[line >> 3] |= 1 << (line & 7);
        /* Jumps to a line that isn't this one or the next: each is roughly
         * an I-cache miss on the Playdate (sequential lines stream in).
         */
        /* (The permanent area, ~first 4KB, is shared and stays cached.) */
        if (line >= 128 && line != ws_lastline && line != ws_lastline + 1)
                ws_jumps++;
        if (line >= 128)
                ws_lastline = line;
        ws_instrs++;
}

static void ws_report(const char *when)
{
        int n = 0;
        for (size_t i = 0; i < sizeof wsbits; i++)
                n += __builtin_popcount(wsbits[i]);
        fprintf(stderr, "[%s] code working set: %d lines = %d KB; %llu line jumps per %llu ARM instrs\n",
                when, n, n * 32 / 1024, (unsigned long long)ws_jumps, (unsigned long long)ws_instrs);
        memset(wsbits, 0, sizeof wsbits);
        ws_jumps = ws_instrs = 0;
}


static void hook_ring(uc_engine *u, uint64_t address, uint32_t size, void *ud)
{
        (void)ud;
        ring[ringi++ & 63] = (uint32_t)address;
        uint16_t h = 0;
        uc_mem_read(u, address, &h, 2);
        if (h == 0xbf30) {
                fprintf(stderr, "WFI at %08llx; last PCs:\n", (unsigned long long)address);
                for (int i = 0; i < 64; i++)
                        fprintf(stderr, " %08x", ring[(ringi + i) & 63]);
                fprintf(stderr, "\n");
                exit(2);
        }
}

/* Block-entry trace for diagnosing mismatches in long chained runs */
#define MAXB 200000
static uint32_t *blk_pc;                /* target addr -> 68k pc, indexed by (taddr - T_CODE) / 2 */
static uint32_t entered[8192], nentered;
static uint32_t mus_pcs[8192], nmus_pcs;
static void observe_translate(uint32_t pc, void *code, uint32_t len)
{
        (void)len;
        uint32_t i = (plat_taddr(code) - T_CODE) / 2;
        blk_pc[i] = pc | 0x80000000u;   /* entered at its push */
        blk_pc[i + 1] = pc | 0x40000000u;       /* entered past its push (chain/jump cache) */
}
static void hook_entry(uc_engine *u, uint64_t address, uint32_t size, void *ud)
{
        (void)u; (void)size; (void)ud;
        uint32_t i = (uint32_t)(address - T_CODE) / 2;
        if (blk_pc[i] && nentered < 8192)
                entered[nentered++] = blk_pc[i];
}

/* ---- I/O record & replay ---- */
extern int (*umac_io_hook)(int write, unsigned int addr, unsigned int value, unsigned int *out);
#define MAXIO 4096
static struct { uint8_t write; uint32_t addr; uint8_t val; uint32_t int_level, virq, nmi; } iolog[MAXIO];
static int nio, ioplay, io_mode;         /* 0 off, 1 record, 2 replay */
static int io_bad, io_unverifiable;
static char io_why[160];

static int io_hook(int write, unsigned int addr, unsigned int value, unsigned int *out)
{
        addr &= 0xffffff;
        if (addr == 0xc00069) {                 /* paravirtual disk: too much going on */
                io_unverifiable = 1;
                if (io_mode == 2)
                        return 1;
                return 0;
        }
        if (io_mode == 1) {
                if (write == 0 || write == 1)
                        return 0;               /* do it; record it after */
                if (nio < MAXIO) {
                        iolog[nio].write = write == 3;
                        iolog[nio].addr = addr;
                        iolog[nio].val = value;
                        /* The device may have changed the interrupt lines */
                        iolog[nio].int_level = m68ki_cpu.int_level;
                        iolog[nio].virq = m68ki_cpu.virq_state;
                        iolog[nio].nmi = m68ki_cpu.nmi_pending;
                        nio++;
                } else {
                        io_unverifiable = 1;
                }
                return 0;
        }
        if (io_mode == 2) {
                if (write >= 2)
                        return 0;
                if (ioplay >= nio || iolog[ioplay].write != (write == 1) || iolog[ioplay].addr != addr ||
                    (write == 1 && iolog[ioplay].val != (value & 0xff))) {
                        if (!io_bad)
                                snprintf(io_why, sizeof io_why, " IO#%d mus %s %06x=%02x vs jit %s",
                                         ioplay, write == 1 ? "W" : "R", addr, value & 0xff,
                                         ioplay < nio ? (iolog[ioplay].write ? "W" : "R") : "(none)");
                        io_bad = 1;
                        if (out)
                                *out = 0;
                        return 1;
                }
                if (out)
                        *out = iolog[ioplay].val;
                m68ki_cpu.int_level = iolog[ioplay].int_level;
                m68ki_cpu.virq_state = iolog[ioplay].virq;
                m68ki_cpu.nmi_pending = iolog[ioplay].nmi;
                ioplay++;
                return 1;
        }
        return 0;
}

/* ---- the lockstep run ---- */

static int jflag_bits(const jregs_t *j)
{
        return ((j->x & 0x100) ? 16 : 0) | ((j->n & 0x80) ? 8 : 0) |
               (j->not_z ? 0 : 4) | ((j->v & 0x80) ? 2 : 0) | ((j->c & 0x100) ? 1 : 0);
}

static int flag_bits(const m68ki_cpu_core *c)
{
        return ((c->x_flag & 0x100) ? 16 : 0) | ((c->n_flag & 0x80) ? 8 : 0) |
               (c->not_z_flag ? 0 : 4) | ((c->v_flag & 0x80) ? 2 : 0) | ((c->c_flag & 0x100) ? 1 : 0);
}

static void disasm_block(uint32_t pc, int n)
{
        char buf[100];
        for (int i = 0; i < n; i++) {
                int len = m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
                fprintf(stderr, "      %06x: %s\n", pc, buf);
                pc += len;
        }
}

static uint32_t plat_run(void *entry, void *code, jregs_t *regs)
{
        static m68ki_cpu_core s0;
        static jregs_t j0, j1;
        m68k_jit_sync_out();                    /* Musashi state = JIT state */
        memcpy(&s0, &m68ki_cpu, sizeof s0);
        j0 = *regs;
        njit_w = 0;
        io_touched = 0;

        uint32_t sp = T_STACK + 0x10000 - 64, lr = T_RET | 1;
        uc_reg_write(uc, UC_ARM_REG_SP, &sp);
        uc_reg_write(uc, UC_ARM_REG_LR, &lr);
        uint32_t target = plat_taddr(code) | 1, rt = plat_taddr(regs);
        uc_reg_write(uc, UC_ARM_REG_R0, &target);
        uc_reg_write(uc, UC_ARM_REG_R1, &rt);
        nentered = 0;
        nio = ioplay = io_bad = io_unverifiable = 0;
        io_mode = 1;
        recording_jit = 1;
        uc_err err = uc_emu_start(uc, plat_taddr(entry) | 1, T_RET, 0, 0);
        recording_jit = 0;
        io_mode = 0;
        /* Helper-made writes: the new value is whatever's there now, as
         * long as nothing later overwrote it (later entries win anyway).
         */
        for (int i = 0; i < njit_w; i++)
                if (jit_w[i].from_helper)
                        jit_w[i].new_ = ram[jit_w[i].addr];
        if (err) {
                uint32_t pc;
                uc_reg_read(uc, UC_ARM_REG_PC, &pc);
                fprintf(stderr, "unicorn: %s at %08x (block for 68k pc %06x)\n", uc_strerror(err), pc, s0.pc);
                if (getenv("RING")) {
                        for (int i = 0; i < 64; i++) {
                                uint32_t a = ring[(ringi + i) & 63];
                                uint32_t bp = (blk_pc && a >= T_CODE && a < T_CODE + CODE_SIZE) ? blk_pc[(a - T_CODE) / 2] : 0;
                                fprintf(stderr, " %08x%s", a, bp ? "*" : "");
                                if (bp)
                                        fprintf(stderr, "(%06x)", bp & 0x3fffffff);
                        }
                        fprintf(stderr, "\n");
                }
                disasm_block(s0.pc, 8);
                exit(1);
        }
        /* Instructions executed: what came off the budget (incl. chained blocks) */
        j1 = *regs;
        uint32_t n = j0.budget - j1.budget;

        if (io_touched || io_unverifiable || njit_w >= 65536) {
                unverified++;
                return n;
        }

        for (int i = njit_w - 1; i >= 0; i--)
                ram[jit_w[i].addr] = jit_w[i].old;
        memcpy(&m68ki_cpu, &s0, sizeof s0);

        int saved_cycles = m68ki_remaining_cycles;
        nmus_w = 0;
        recording_mus = 1;
        io_mode = 2;
        nmus_pcs = 0;
        for (uint32_t i = 0; i < n; i++) {
                if (iexec)
                        iexec[m68ki_cpu.pc & 0xffffff]++;
                if (nmus_pcs < 8192)
                        mus_pcs[nmus_pcs++] = m68ki_cpu.pc;
                m68k_step_one();
        }
        recording_mus = 0;
        io_mode = 0;
        m68ki_remaining_cycles = saved_cycles;

        int bad = 0;
        char why[512] = "";
        int wl = 0;
        if (io_bad || ioplay != nio) {
                bad = 1;
                wl += snprintf(why + wl, sizeof why - wl, "%s (I/O replayed %d of %d)", io_why, ioplay, nio);
        }
        for (int i = 0; i < 16; i++) {
                if (j1.dar[i] != m68ki_cpu.dar[i]) {
                        bad = 1;
                        wl += snprintf(why + wl, sizeof why - wl, " %c%d jit=%08x mus=%08x",
                                       i < 8 ? 'D' : 'A', i & 7, j1.dar[i], m68ki_cpu.dar[i]);
                }
        }
        if (j1.pc != m68ki_cpu.pc) {
                bad = 1;
                wl += snprintf(why + wl, sizeof why - wl, " PC jit=%06x mus=%06x", j1.pc, m68ki_cpu.pc);
        }
        if (jflag_bits(&j1) != flag_bits(&m68ki_cpu)) {
                bad = 1;
                wl += snprintf(why + wl, sizeof why - wl, " XNZVC jit=%02x mus=%02x",
                               jflag_bits(&j1), flag_bits(&m68ki_cpu));
        }
        /* Memory: every byte either side wrote must agree */
        for (int i = 0; i < njit_w && !bad; i++) {
                uint32_t a = jit_w[i].addr;
                uint8_t jv = jit_w[i].new_;
                for (int k = i + 1; k < njit_w; k++)
                        if (jit_w[k].addr == a)
                                jv = jit_w[k].new_;
                if (ram[a] != jv) {
                        bad = 1;
                        wl += snprintf(why + wl, sizeof why - wl, " mem[%06x] jit=%02x mus=%02x", a, jv, ram[a]);
                }
        }
        for (int i = 0; i < nmus_w && !bad; i++) {
                for (int b = 0; b < 4; b++) {
                        uint32_t a = mus_w[i].addr + b;
                        if (a >= RAM_SIZE)
                                continue;
                        int found = 0;
                        uint8_t jv = 0;
                        for (int k = 0; k < njit_w; k++)
                                if (jit_w[k].addr == a) {
                                        found = 1;
                                        jv = jit_w[k].new_;
                                }
                        if (!found) {
                                /* JIT left it alone; it should still hold the old value */
                                uint8_t old = mus_w[i].old[b];
                                for (int j = 0; j < i; j++)     /* earliest recorded old */
                                        if (mus_w[j].addr <= a && a < mus_w[j].addr + 4) {
                                                old = mus_w[j].old[a - mus_w[j].addr];
                                                break;
                                        }
                                jv = old;
                        }
                        if (ram[a] != jv) {
                                bad = 1;
                                wl += snprintf(why + wl, sizeof why - wl, " mem[%06x] jit=%02x mus=%02x", a, jv, ram[a]);
                                break;
                        }
                }
        }

        if (bad) {
                mismatches++;
                if (mismatches <= (uint64_t)max_report) {
                        fprintf(stderr, "MISMATCH block %06x (%u instrs):%s\n", s0.pc, n, why);
                        if (n <= 40)
                                disasm_block(s0.pc, n);
                        if (blk_pc) {
                                /* Walk the JIT's block entries against Musashi's pcs */
                                uint32_t m = 0;
                                for (uint32_t k = 0; k < nentered; k++) {
                                        uint32_t want = entered[k] & 0x3fffffff;
                                        while (m < nmus_pcs && mus_pcs[m] != want)
                                                m++;
                                        if (m >= nmus_pcs) {
                                                fprintf(stderr, "  JIT entered block %06x (%s) at entry %u; Musashi never went there."
                                                        " Previous JIT block %06x\n", want,
                                                        entered[k] & 0x80000000u ? "via push" : "chained/cached", k,
                                                        k ? entered[k - 1] & 0x3fffffff : 0);
                                                if (k)
                                                        disasm_block(entered[k - 1] & 0x3fffffff, 12);
                                                break;
                                        }
                                }
                        }
                }
        } else {
                verified++;
        }
        /* Carry on from Musashi's result, keeping the JIT's bookkeeping */
        m68k_jit_sync_in();
        regs->budget = j1.budget;
        regs->lastexit = j1.lastexit;
        return n;
}

/* Which writes cause flushes: histogram by 64-byte line, with the PC */
#define NHIST 4096
static struct { uint32_t line, count, pc; } fh[NHIST];
static void observe_flush(uint32_t addr)
{
        uint32_t line = addr & ~63u;
        for (int i = 0; i < NHIST; i++) {
                if (fh[i].count == 0) {
                        fh[i].line = line; fh[i].count = 1; fh[i].pc = m68ki_cpu.ppc;
                        return;
                }
                if (fh[i].line == line) {
                        fh[i].count++;
                        return;
                }
        }
}

static void flush_report(void)
{
        for (int k = 0; k < 12; k++) {
                int best = -1;
                for (int i = 0; i < NHIST && fh[i].count; i++)
                        if (best < 0 || fh[i].count > fh[best].count)
                                best = i;
                if (best < 0 || !fh[best].count)
                        break;
                fprintf(stderr, "  flush line %06x: %u (writer pc ~%06x)\n", fh[best].line, fh[best].count, fh[best].pc);
                fh[best].count = 0;
        }
}

/* Which instructions fall back to the interpreter */
static uint32_t interp_hist[65536], interp_pc[65536];
static void observe_interp(uint32_t pc, uint32_t op)
{
        interp_hist[op & 0xffff]++;
        interp_pc[op & 0xffff] = pc;
}

static void interp_report(void)
{
        uint64_t total = 0;
        for (int i = 0; i < 65536; i++)
                total += interp_hist[i];
        fprintf(stderr, "interpreted: %llu instrs; top opcodes:\n", (unsigned long long)total);
        for (int k = 0; k < 25; k++) {
                int best = 0;
                for (int i = 1; i < 65536; i++)
                        if (interp_hist[i] > interp_hist[best])
                                best = i;
                if (!interp_hist[best])
                        break;
                char buf[100];
                m68k_disassemble(buf, interp_pc[best], M68K_CPU_TYPE_68000);
                fprintf(stderr, "  %04x %8u (%4.1f%%)  e.g. %06x: %s\n", best, interp_hist[best],
                        100.0 * interp_hist[best] / total, interp_pc[best], buf);
                interp_hist[best] = 0;
        }
}

/* Code size per 68k instruction, weighted by how often it executes */
static uint16_t *isize_at;              /* by 68k pc (24-bit): native bytes of its translation */
static uint16_t *iop_at;
static void observe_size(uint32_t pc, uint32_t op, uint32_t bytes)
{
        isize_at[pc & 0xffffff] = bytes;
        iop_at[pc & 0xffffff] = op;
}

static void size_report(void)
{
        /* Group by top 4 bits + mode-ish bits for a readable breakdown */
        static double bytes_by[16], execs_by[16];
        double tb = 0, te = 0;
        for (uint32_t pc = 0; pc < 0x1000000; pc++) {
                if (!iexec[pc] || !isize_at[pc])
                        continue;
                int line = iop_at[pc] >> 12;
                bytes_by[line] += (double)iexec[pc] * isize_at[pc];
                execs_by[line] += iexec[pc];
                tb += (double)iexec[pc] * isize_at[pc];
                te += iexec[pc];
        }
        /* Top individual instructions by bytes run */
        for (int k = 0; k < 25; k++) {
                uint32_t best = 0;
                double bv = 0;
                for (uint32_t pc = 0; pc < 0x1000000; pc++) {
                        double v = (double)iexec[pc] * isize_at[pc];
                        if (v > bv) { bv = v; best = pc; }
                }
                if (!bv)
                        break;
                char buf[100];
                m68k_disassemble(buf, best, M68K_CPU_TYPE_68000);
                fprintf(stderr, "  %06x %-28s x%-7u %3u B  %4.1f%%\n", best, buf, iexec[best], isize_at[best], 100 * bv / tb);
                iexec[best] = 0;
        }
        static const char *names[16] = { "0 bit/imm", "1 move.b", "2 move.l", "3 move.w", "4 misc", "5 addq/scc/dbcc",
                "6 bcc/bsr", "7 moveq", "8 or/div", "9 sub", "a aline", "b cmp/eor", "c and/mul", "d add", "e shift", "f" };
        fprintf(stderr, "executed code bytes: %.1f per 68k instr overall\n", tb / te);
        for (int i = 0; i < 16; i++)
                if (execs_by[i])
                        fprintf(stderr, "  %-16s %5.1f%% of instrs, %5.1f bytes each, %5.1f%% of code bytes run\n",
                                names[i], 100 * execs_by[i] / te, bytes_by[i] / execs_by[i], 100 * bytes_by[i] / tb);
}

/* ---- boot + script ---- */

static uint64_t t_us, vs = 16626, hz = 1000000;

static void run_ms(int ms)
{
        uint64_t end = t_us + ms * 1000ull;
        while (t_us < end) {
                umac_loop();
                t_us += 5000;
                while (t_us >= vs) { umac_vsync_event(); vs += 16626; }
                while (t_us >= hz) { umac_1hz_event(); hz += 1000000; }
        }
}

static void frame(int x, int y, int button)
{
        mac_set_mouse(x, y);
        umac_mouse(0, 0, button);
        run_ms(25);
}

static uint8_t *slurp(const char *p, size_t *n)
{
        FILE *f = fopen(p, "rb");
        if (!f) { perror(p); exit(1); }
        fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc(*n);
        if (fread(b, 1, *n, f) != *n) exit(1);
        fclose(f);
        return b;
}

static void snapshot(const char *prefix, const char *name)
{
        char path[512];
        snprintf(path, sizeof path, "%s_%s.bin", prefix, name);
        FILE *o = fopen(path, "wb");
        const uint8_t *fb = ram + umac_get_fb_offset();
        for (int i = 0; i < DISP_WIDTH * DISP_HEIGHT / 8; i++)
                fputc(~fb[i] & 0xff, o);
        fclose(o);
}

static void report(const char *when)
{
        m68kjit_stats_t *s = &m68k_jit_stats;
        double tot = (double)(s->jit_instrs + s->interp_instrs);
        fprintf(stderr, "[%s] t=%.1fs  jit %llu (%.1f%%)  interp %llu  blocks %llu  avg %.1f/blk  "
                "translations %u (conflict %u stale %u recycles %u, %.0f B/blk, chains %u)  flushes %u  verified %llu  unverified %llu  MISMATCHES %llu\n",
                when, t_us / 1e6, (unsigned long long)s->jit_instrs, tot ? 100.0 * s->jit_instrs / tot : 0,
                (unsigned long long)s->interp_instrs, (unsigned long long)s->blocks,
                s->blocks ? (double)s->jit_instrs / s->blocks : 0,
                s->translations, s->conflicts, s->stale, s->recycles,
                s->translations ? (double)s->code_bytes / s->translations : 0, s->chains, s->flushes, (unsigned long long)verified,
                (unsigned long long)unverified, (unsigned long long)mismatches);
}

int main(int argc, char **argv)
{
        if (argc < 4) {
                fprintf(stderr, "usage: %s rom.bin disk.img outprefix [script]\n", argv[0]);
                return 1;
        }
        size_t rn, dn;
        uint8_t *rom_file = slurp(argv[1], &rn);
        rom = aligned(ROM_SIZE);
        memcpy(rom, rom_file, ROM_SIZE);
        if (rom_patch(rom)) { puts("rom_patch failed"); return 1; }
        ram = aligned(RAM_SIZE);
        ram[0x2af] = 0x40;      /* warm boot: skip RAM test */

        disc_descr_t d[DISC_NUM_DRIVES] = {0};
        d[0].base = slurp(argv[2], &dn);
        d[0].size = dn;
        umac_init(ram, rom, d);

        uc_err err = uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, &uc);
        if (err) die("uc_open", err);
        uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M7);

        static const m68kjit_platform_t plat = {
                .taddr = plat_taddr, .helper_addr = plat_helper_addr, .run = plat_run,
                .code_written = plat_code_written, .recycle = plat_recycle, .alloc = plat_alloc,
                .regs = plat_regs,
        };
        jregs_buf = aligned(0x1000);
        cpu_page = (uintptr_t)&m68ki_cpu & ~(uintptr_t)0xfff;
        cpu_page_len = (((uintptr_t)&m68ki_cpu + sizeof m68ki_cpu + 0xfff) & ~(uintptr_t)0xfff) - cpu_page;
        if (m68k_jit_init(&plat, ram, RAM_SIZE, rom, ROM_SIZE, CODE_SIZE)) {
                puts("jit init failed");
                return 1;
        }
        codepage_ptr = allocs[0];
        code_ptr = allocs[2];
        m68k_jit_write_observer = observe_write;
        umac_io_hook = io_hook;
        m68k_jit_flush_observer = observe_flush;

        cpu_page = (uintptr_t)&m68ki_cpu & ~(uintptr_t)0xfff;
        cpu_page_len = (((uintptr_t)&m68ki_cpu + sizeof m68ki_cpu + 0xfff) & ~(uintptr_t)0xfff) - cpu_page;

        if ((err = uc_mem_map_ptr(uc, T_RAM, RAM_SIZE, UC_PROT_ALL, ram))) die("map ram", err);
        if ((err = uc_mem_map_ptr(uc, T_ROM, ROM_SIZE, UC_PROT_ALL, rom))) die("map rom", err);
        if ((err = uc_mem_map_ptr(uc, T_CPU, cpu_page_len, UC_PROT_ALL, (void *)cpu_page))) die("map cpu", err);
        if ((err = uc_mem_map_ptr(uc, T_MAP, alloc_len[0], UC_PROT_ALL, codepage_ptr))) die("map codepage", err);
        if ((err = uc_mem_map_ptr(uc, T_CODE, CODE_SIZE, UC_PROT_ALL, code_ptr))) die("map code", err);
        if ((err = uc_mem_map(uc, T_HELP, 0x1000, UC_PROT_ALL))) die("map help", err);
        if ((err = uc_mem_map_ptr(uc, T_REGS, 0x4000, UC_PROT_ALL, jregs_buf))) die("map regs", err);
        if ((err = uc_mem_map(uc, T_STACK, 0x10000, UC_PROT_ALL))) die("map stack", err);
        if ((err = uc_mem_map(uc, T_RET, 0x1000, UC_PROT_ALL))) die("map ret", err);
        uint16_t bxlr[2 * JH_COUNT];
        for (int i = 0; i < 2 * JH_COUNT; i++)
                bxlr[i] = 0x4770;
        uc_mem_write(uc, T_HELP, bxlr, sizeof bxlr);

        uc_hook h1, h2, h3;
        uc_hook h4, h5, h6, h7, h8;
        if (getenv("SIM")) {
                ophist_on = getenv("OPHIST") != NULL;
                cache_init(&icache, 16384, 2);
                cache_init(&dcache, 16384, 4);
                uc_hook_add(uc, &h7, UC_HOOK_CODE, sim_fetch, NULL, 1, 0);
                uc_hook_add(uc, &h8, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, sim_data, NULL, 1, 0);
        }
        if (getenv("BTRACE")) {
                blk_pc = calloc(CODE_SIZE / 2, sizeof *blk_pc);
                m68k_jit_translate_observer = observe_translate;
                uc_hook_add(uc, &h6, UC_HOOK_CODE, hook_entry, NULL, T_CODE, T_CODE + CODE_SIZE - 1);
        }
        if (getenv("WS"))
                uc_hook_add(uc, &h5, UC_HOOK_CODE, hook_ws, NULL, T_CODE, T_CODE + CODE_SIZE - 1);
        if (getenv("RING"))
                uc_hook_add(uc, &h4, UC_HOOK_CODE, hook_ring, NULL, 1, 0);
        if (getenv("TRACE"))
                uc_hook_add(uc, &h3, UC_HOOK_CODE, hook_trace, NULL, 1, 0);
        uc_hook_add(uc, &h1, UC_HOOK_CODE, hook_helper, NULL, T_HELP, T_HELP + 4 * JH_COUNT - 1);
        uc_hook_add(uc, &h2, UC_HOOK_MEM_WRITE, hook_mem_write, NULL, T_RAM, T_RAM + RAM_SIZE - 1);

        if (getenv("DUMP")) {
                extern uint16_t *m68k_jit_debug_translate(uint32_t pc, uint32_t *len);
                uint32_t len, pc = strtoul(getenv("DUMP"), 0, 16);
                overlay = 0;
                uint16_t *c = m68k_jit_debug_translate(pc, &len);
                FILE *o = fopen(getenv("DUMPFILE"), "wb");
                fwrite(c, 1, len, o);
                fclose(o);
                fprintf(stderr, "dumped %u bytes for block %06x\n", len, pc);
                disasm_block(pc, 8);
                return 0;
        }
        extern int m68k_jit_no_jcache;
        m68k_jit_no_jcache = getenv("NOJC") != NULL;
        extern int m68k_jit_no_follow;
        m68k_jit_no_follow = getenv("NOFOLLOW") != NULL;
        extern int m68k_jit_fused_ea;
        m68k_jit_fused_ea = getenv("FUSED") != NULL;
        extern int m68k_jit_no_traces;
        m68k_jit_no_traces = getenv("NOTRACE") != NULL;
        extern int m68k_jit_max_budget;
        if (getenv("BUDGET"))
                m68k_jit_max_budget = atoi(getenv("BUDGET"));
        if (getenv("MICRO")) {
                /* Same loops as the device microbenchmark (main.c, MARBLE_BENCH) */
                static const uint16_t reg_loop[] = {
                        0x303C, 0x7FFF, 0xD481, 0xD682, 0xB781, 0x5281, 0xD481, 0xD682, 0xB781, 0x5281,
                        0xD481, 0xD682, 0xB781, 0x5281, 0xD481, 0xD682, 0xB781, 0x5281, 0x51C8, 0xFFDE, 0x60FE,
                };
                static const uint16_t mem2[] = {
                        0x2049, 0x303C, 0x00FF, 0x2218, 0xD481, 0x2218, 0xD481, 0x2218, 0xD481, 0x2218, 0xD481,
                        0x51C8, 0xFFEE, 0x60E4,
                };
                uint16_t mem3[sizeof mem2 / 2];
                memcpy(mem3, mem2, sizeof mem2);
                mem3[2] = 0x3FFF;
                struct { const char *name; const uint16_t *code; int n; uint32_t a1; } tests[] = {
                        { "registers", reg_loop, sizeof reg_loop / 2, 0 },
                        { "mem4K", mem2, sizeof mem2 / 2, 0x20000 },
                        { "mem256K", mem3, sizeof mem3 / 2, 0x20000 },
                };
                for (unsigned k = 0; k < 3; k++) {
                        for (int i = 0; i < tests[k].n; i++) {
                                ram[0x10000 + i * 2] = tests[k].code[i] >> 8;
                                ram[0x10000 + i * 2 + 1] = tests[k].code[i] & 0xff;
                        }
                        overlay = 0;
                        m68k_set_reg(M68K_REG_PC, 0x10000);
                        m68k_set_reg(M68K_REG_A1, tests[k].a1);
                        m68k_jit_execute(8 * 1000);
                        sim_report("warmup");
                        m68k_jit_stats.jit_instrs = 0;
                        m68k_jit_execute(8 * 400000);
                        fprintf(stderr, "%s: %llu instrs ", tests[k].name, (unsigned long long)m68k_jit_stats.jit_instrs);
                        sim_report(tests[k].name);
                }
                return 0;
        }
        int booted_ms = 0;
        while (!mac_booted()) { run_ms(100); booted_ms += 100; if (booted_ms > 60000) break; }
        report("booted");

        int x = 15, y = 15;
        for (int i = 4; i < argc; i++) {
                if (!strcmp(argv[i], "m")) {
                        int tx = atoi(argv[++i]), ty = atoi(argv[++i]);
                        for (int k = 1; k <= 20; k++)
                                frame(x + (tx - x) * k / 20, y + (ty - y) * k / 20, 0);
                        x = tx; y = ty;
                } else if (!strcmp(argv[i], "c") || !strcmp(argv[i], "dc")) {
                        int nclick = argv[i][0] == 'd' ? 2 : 1;
                        for (int k = 0; k < nclick; k++) {
                                for (int j = 0; j < 3; j++) frame(x, y, 1);
                                for (int j = 0; j < 3; j++) frame(x, y, 0);
                        }
                } else if (!strcmp(argv[i], "w")) {
                        int ms = atoi(argv[++i]);
                        for (int j = 0; j < ms / 25; j++) frame(x, y, 0);
                } else if (!strcmp(argv[i], "t")) {
                        run_ms((int)(atof(argv[++i]) * 1000));
                } else if (!strcmp(argv[i], "ih")) {
                        if (!m68k_jit_interp_observer)
                                m68k_jit_interp_observer = observe_interp;
                        else
                                interp_report();
                } else if (!strcmp(argv[i], "sizes")) {
                        if (!iexec) {
                                extern void (*m68k_jit_size_observer)(uint32_t, uint32_t, uint32_t);
                                isize_at = calloc(0x1000000, 2);
                                iop_at = calloc(0x1000000, 2);
                                iexec = calloc(0x1000000, 4);
                                m68k_jit_size_observer = observe_size;
                        } else {
                                size_report();
                        }
                } else if (!strcmp(argv[i], "sim")) {
                        sim_report(argv[++i]);
                } else if (!strcmp(argv[i], "ws")) {
                        ws_report(argv[++i]);
                } else if (!strcmp(argv[i], "s")) {
                        snapshot(argv[3], argv[++i]);
                        report(argv[i]);
                }
        }
        report("end");
        flush_report();
        return mismatches != 0;
}
