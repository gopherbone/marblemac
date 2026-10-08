/* Playdate (device) platform layer for the 68k JIT.
 *
 * Measured on a Rev B: freshly written code at never-executed cache lines
 * runs fine with just DSB/ISB (no D-cache maintenance needed), but the
 * I-cache will happily serve stale copies of lines that have executed.
 * So the JIT only ever writes code to fresh lines, and before it reuses
 * its buffer we run a big NOP sled to push old lines out of the I-cache.
 * (Games run unprivileged, so the real cache-maintenance registers fault.)
 *
 * The D-cache is write-back without write-allocate: a store to a line
 * that isn't cached goes straight to memory, but a store to a cached line
 * stays in the cache, invisible to instruction fetch.  The JIT never reads
 * its code, so the only way code lines get cached is from whatever the
 * memory was used for before; we sweep the D-cache once after allocating
 * the buffer (and on recycle) so none are.
 *
 * Likewise the I-cache can hold lines for our heap addresses from code
 * that ran there before the game launched (the launcher, the OS), so we
 * flush it with a NOP sled before running the first block.  The sled is
 * part of the game binary, which the OS made safe to run when loading it;
 * a sled on the heap could hit those same stale lines.
 */
#ifdef UMAC_JIT

#include <stdint.h>
#include "pd_api.h"
#include "m68kjit.h"
#include "jit_playdate.h"
#include "jit/thumb.h"

#define CODE_SIZE       (2u << 20)

#define SWEEP_BYTES     (512u << 10)    /* 32x the D-cache */

static PlaydateAPI *pd;

/* Even code at never-before-run addresses can come out stale: lines just
 * past executed code get fetched speculatively before we've written them.
 * (Measured: crashes stop once the I-cache is flushed after each
 * translation, and don't need the D-cache swept.)
 *
 * So after writing a block we evict exactly its lines.  The sled is 16
 * copies of an 8KB stretch, each 32-byte line being 15 NOPs and a BX LR,
 * so any single line can be run on its own.  Running the same line offset
 * in every copy hits the same I-cache set 16 times (the way size is at
 * most 8KB), which leaves no stale line standing.
 */
#define SLED_WAY        8192
#define SLED_COPIES     16

__attribute__((naked, noinline, aligned(SLED_WAY))) static void icache_sled(void)
{
        __asm__ volatile (
                ".rept 4096\n"
                " .rept 15\n nop\n .endr\n"
                " bx lr\n"
                ".endr\n");
}

static void evict_icache_lines(const void *p, uint32_t len)
{
        uintptr_t base = (uintptr_t)icache_sled & ~(uintptr_t)1;
        uintptr_t a = (uintptr_t)p & ~(uintptr_t)31, end = (uintptr_t)p + len;
        for (; a < end; a += 32) {
                uintptr_t off = a & (SLED_WAY - 1);
                for (int c = 0; c < SLED_COPIES; c++)
                        ((void (*)(void))((base + c * SLED_WAY + off) | 1))();
        }
        __asm__ volatile ("dsb\n isb" ::: "memory");
}

/* Whole I-cache: every line of the sled, twice */
static void flush_icache(void)
{
        uintptr_t base = (uintptr_t)icache_sled & ~(uintptr_t)1;
        for (int pass = 0; pass < 2; pass++)
                for (uint32_t off = 0; off < SLED_WAY * SLED_COPIES; off += 32)
                        ((void (*)(void))((base + off) | 1))();
        __asm__ volatile ("dsb\n isb" ::: "memory");
}

/* Push every line out of the D-cache by reading lots of other memory */
static void sweep_dcache(void)
{
        volatile uint32_t *buf = pd->system->realloc(NULL, SWEEP_BYTES);
        if (!buf)
                return;
        uint32_t acc = 0;
        for (uint32_t i = 0; i < SWEEP_BYTES / 4; i += 8)
                acc += buf[i];
        pd->system->realloc((void *)buf, 0);
        __asm__ volatile ("dsb\n isb" ::: "memory");
        (void)acc;
}

static uint32_t taddr(const void *p)
{
        return (uint32_t)(uintptr_t)p;
}

static uint32_t helper_addr(int n)
{
        return (uint32_t)(uintptr_t)m68k_jit_helper_fn[n];     /* Thumb bit already set */
}

static uint32_t run(void *entry, void *code, jregs_t *regs)
{
        return ((uint32_t (*)(uint32_t, jregs_t *))((uintptr_t)entry | 1))((uint32_t)(uintptr_t)code | 1, regs);
}

static uint32_t run_raw(void *code)
{
        return ((uint32_t (*)(void))((uintptr_t)code | 1))();
}

static void code_written(void *p, uint32_t len)
{
        evict_icache_lines(p, len);
}

static void recycle(void)
{
        sweep_dcache();
        flush_icache();
}

static void *alloc(size_t n)
{
        return pd->system->realloc(NULL, n);
}

static float now(void)
{
        return pd->system->getElapsedTime();
}

#ifdef JIT_DEBUG_LOG
/* Log every translation, and checksum each block before it runs */
#define NCHK 16384
static struct { void *code; uint32_t len, sum; } chk[NCHK];
static int nchk;

static uint32_t sum16(const uint16_t *p, uint32_t len)
{
        uint32_t s = 0;
        for (uint32_t i = 0; i < len / 2; i++)
                s = s * 31 + p[i];
        return s;
}

static void observe_translate(uint32_t pc, void *code, uint32_t len)
{
        pd->system->logToConsole("xl %p %06lx %lu", code, (unsigned long)pc, (unsigned long)len);
        if (nchk < NCHK) {
                chk[nchk].code = code;
                chk[nchk].len = len;
                chk[nchk].sum = sum16(code, len);
                nchk++;
        }
}

static uint32_t run_checked(void *entry, void *code, jregs_t *regs)
{
        for (int i = nchk - 1; i >= 0; i--) {
                if (chk[i].code == code) {
                        if (sum16(code, chk[i].len) != chk[i].sum)
                                pd->system->logToConsole("CODE CORRUPTED at %p", code);
                        break;
                }
        }
        return run(entry, code, regs);
}
#endif

static const m68kjit_platform_t plat = {
        .taddr = taddr,
        .helper_addr = helper_addr,
#ifdef JIT_DEBUG_LOG
        .run = run_checked,
#else
        .run = run,
#endif
        .code_written = code_written,
        .recycle = recycle,
        .alloc = alloc,
#ifdef MARBLE_PROFILE
        .now = now,
#endif
};

int jit_playdate_init(PlaydateAPI *api, uint8_t *ram, uint32_t ram_size,
                      const uint8_t *rom, uint32_t rom_size)
{
        pd = api;

        /* Self-test: load each base pointer the JIT will bake into code */
        extern char m68ki_cpu[];
        uint32_t vals[] = { taddr(ram), taddr(rom), taddr(m68ki_cpu), 0x12345678, 0x90354000 };
        uint16_t *buf = alloc(64 * 6 * 2 + 64);
        sweep_dcache();
        flush_icache();
        int bad = 0;
        for (unsigned i = 0; i < sizeof vals / sizeof *vals; i++) {
                temit_t e = { .p = buf + 64 * i, .start = buf + 64 * i, .end = buf + 64 * i + 32 };
                t_mov32(&e, R0, vals[i]);
                t_bx(&e, LR);
                __asm__ volatile ("dsb\n isb" ::: "memory");
                uint32_t got = run_raw(buf + 64 * i);
                if (got != vals[i]) {
                        bad++;
                        pd->system->logToConsole("jit selftest: mov32 %08x gave %08x (code %04x %04x %04x %04x)",
                                                 vals[i], got, buf[64 * i], buf[64 * i + 1], buf[64 * i + 2], buf[64 * i + 3]);
                }
        }
        pd->system->logToConsole("jit selftest: %d bad; ram %p rom %p cpu %p", bad, (void *)ram, (void *)rom, (void *)m68ki_cpu);
        if (bad)
                return -1;
#ifdef JIT_DEBUG_LOG
        m68k_jit_translate_observer = observe_translate;
#endif
        int r = m68k_jit_init(&plat, ram, ram_size, rom, rom_size, CODE_SIZE);
        sweep_dcache();
        flush_icache();
        return r;
}

#endif
