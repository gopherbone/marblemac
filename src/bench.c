/* One-off device characterisation, built with -DMARBLE_BENCH: where
 * things live in memory, and what a cache miss costs.
 */
#ifdef MARBLE_BENCH
#include <stdint.h>
#include "pd_api.h"
#include "jit/thumb.h"

extern void (*const m68ki_static_instruction_jump_table[0x10000])(void);
int m68k_execute(int num_cycles);

static uint32_t walk(PlaydateAPI *pd, volatile uint32_t *buf, uint32_t n_words, uint32_t steps, float *ns_per)
{
        /* Pointer chase with a 64-byte stride, scrambled, so each step is
         * a dependent load that the cache only helps if it all fits.
         */
        for (uint32_t i = 0; i < n_words; i++)
                buf[i] = 0;
        uint32_t lines = n_words / 16, idx = 0;
        for (uint32_t i = 0; i < lines; i++) {
                uint32_t next = (idx + 7919) % lines;
                buf[idx * 16] = next * 16;
                idx = next;
        }
        float t0 = pd->system->getElapsedTime();
        uint32_t p = 0;
        for (uint32_t i = 0; i < steps; i++)
                p = buf[p];
        float t1 = pd->system->getElapsedTime();
        *ns_per = (t1 - t0) * 1e9f / steps;
        return p;
}

/* Can we run code we generate on the heap (i.e. is a JIT possible)?
 * Cortex-M7 needs the D-cache cleaned and the I-cache invalidated first.
 */
/* Games run unprivileged, so the SCB cache-maintenance registers fault.
 * Instead: push the generated code out of the (16KB) D-cache by reading a
 * much bigger buffer, and only ever execute addresses the I-cache has
 * never seen.
 */
static volatile uint32_t *evict_buf;

static void sync_code(void *p, uint32_t len)
{
        (void)p; (void)len;
        uint32_t acc = 0;
        for (int i = 0; i < (64 * 1024) / 4; i += 8)    /* one read per 32B line */
                acc += evict_buf[i];
        __asm__ volatile ("dsb\n isb" ::: "memory");
        (void)acc;
}

/* D-cache: 16KB, 4-way, 32B lines -> 128 sets, 4KB per way.  Evict a
 * line by reading K never-recently-used lines that map to the same set,
 * from a big rotating window so they're always misses.
 */
#define EV_WAYS 512                     /* 2MB window */
static volatile uint8_t *ev_win;
static uint16_t ev_next[128];

static void clean_lines(void *p, uint32_t len, int k)
{
        uintptr_t a = (uintptr_t)p & ~31u, end = (uintptr_t)p + len;
        uint32_t acc = 0;
        for (; a < end; a += 32) {
                uint32_t set = (a >> 5) & 127;
                for (int i = 0; i < k; i++) {
                        uint32_t w = ev_next[set]++ % EV_WAYS;
                        acc += ev_win[w * 4096 + set * 32];
                }
        }
        __asm__ volatile ("dsb\n isb" ::: "memory");
        (void)acc;
}

/* Fill [p, p+n) with "movs r0,#0xEE; bx lr" and push it out to memory */
static void prefill(uint16_t *p, int nhalf)
{
        for (int i = 0; i + 1 < nhalf; i += 2) {
                p[i] = 0x20EE;
                p[i + 1] = 0x4770;
        }
        sync_code(p, nhalf * 2);
        clean_lines(p, nhalf * 2, 64);
}

/* Returns how many of n fresh functions (stride bytes apart) returned the
 * wrong value; k = clean reads per line (0 = no clean).
 */
static int coherency_run(PlaydateAPI *pd, int n, int stride, int k, float *us_per)
{
        uint16_t *buf = pd->system->realloc(NULL, n * stride + 64);
        uint16_t *base = (uint16_t *)(((uintptr_t)buf + 31) & ~31u);
        prefill(base, n * stride / 2);
        int bad = 0;
        float t = 0;
        for (int i = 0; i < n; i++) {
                uint16_t *c = base + i * stride / 2;
                int want = 1 + (i % 200);
                c[0] = 0x2000 | want;           /* movs r0, #want */
                c[1] = 0x4770;
                float t0 = pd->system->getElapsedTime();
                if (k)
                        clean_lines(c, 4, k);
                else
                        __asm__ volatile ("dsb\n isb" ::: "memory");
                t += pd->system->getElapsedTime() - t0;
                int r = ((int (*)(void))((uintptr_t)c | 1))();
                bad += r != want;
        }
        *us_per = t * 1e6f / n;
        /* leak buf: never reuse executed addresses */
        return bad;
}

/* JIT-like stress: fresh ~200-byte functions written sequentially with a
 * forward branch patched after the body, separated by `gap` bytes, each
 * run right away and then re-run among older ones.  Stale fetches return
 * 0xEE (prefill) or the wrong ID.
 */
static int jit_stress(PlaydateAPI *pd, int n, int gap, int far_read)
{
        uint32_t span = n * (256 + gap) + 64;
        uint16_t *buf = pd->system->realloc(NULL, span);
        if (!buf) {
                /* (next to a 4MB Mac the heap runs out after a few rounds) */
                pd->system->logToConsole("bench: stress: no memory for %lu bytes", (unsigned long)span);
                return -1;
        }
        uint16_t *p = (uint16_t *)(((uintptr_t)buf + 31) & ~31u);
        prefill(p, (span - 64) / 2);
        static uint16_t *fn[2048];
        int bad = 0;
        uint32_t acc = 0;
        for (int i = 0; i < n; i++) {
                uint16_t *c = p, *q = c;
                int id = 1 + (i % 250);
                *q++ = 0xB500;                          /* push {lr} */
                uint16_t *br = q; q += 2;               /* b.w over the body (patched below) */
                for (int k = 0; k < 80; k++)
                        *q++ = 0x20EE;                  /* movs r0,#0xEE (skipped) */
                uint16_t *target = q;
                *q++ = 0x2000 | id;                     /* movs r0,#id */
                *q++ = 0xBD00;                          /* pop {pc} */
                tbr_t b = { br, C_AL };
                t_patch_branch(b, target);
                __asm__ volatile ("dsb\n isb" ::: "memory");
                if (far_read)
                        acc += ev_win[(i * 4096 * 7) % (EV_WAYS * 4096)];
                fn[i] = c;
                int r = ((int (*)(void))((uintptr_t)c | 1))();
                bad += r != id;
                /* re-run a few older ones */
                for (int k = 1; k <= 3 && i - k * 7 >= 0; k++) {
                        int j = i - k * 7;
                        int rj = ((int (*)(void))((uintptr_t)fn[j] | 1))();
                        bad += rj != 1 + (j % 250);
                }
                p = (uint16_t *)((((uintptr_t)q + 31) & ~31u) + gap);
        }
        (void)acc;
        return bad;
}

static int probe_step;

void jit_probe_step(PlaydateAPI *pd)
{
        int bad;
        static const int gaps[] = { 32, 128, 256, 512, 1024 };
        int st = probe_step++;
        if (st == 0) {
                evict_buf = pd->system->realloc(NULL, 64 * 1024);
                ev_win = pd->system->realloc(NULL, EV_WAYS * 4096);
                pd->system->logToConsole("bench: stress start");
        } else if (st <= 5) {
                bad = jit_stress(pd, 1500, gaps[st - 1], 0);
                pd->system->logToConsole("bench: stress gap %d: %d bad", gaps[st - 1], bad);
        } else if (st <= 10) {
                bad = jit_stress(pd, 1500, gaps[st - 6], 1);
                pd->system->logToConsole("bench: stress gap %d + far read: %d bad", gaps[st - 6], bad);
        } else if (st == 11) {
                pd->system->logToConsole("bench: stress done");
        }
}

void marble_bench(PlaydateAPI *pd, void *heap_block)
{

        int local;
        pd->system->logToConsole("bench: code %p  jumptable %p  heap %p  stack %p",
                                 (void *)m68k_execute, (void *)m68ki_static_instruction_jump_table,
                                 heap_block, (void *)&local);

        uint32_t *buf = pd->system->realloc(NULL, 1 << 20);
        uint32_t sizes[] = { 2048, 8192, 16384, 65536, 262144, 1 << 20 };
        for (unsigned i = 0; i < sizeof sizes / sizeof *sizes; i++) {
                float ns;
                walk(pd, buf, sizes[i] / 4, 200000, &ns);
                pd->system->logToConsole("bench: dependent load over %7u bytes: %.1f ns", sizes[i], (double)ns);
        }
        pd->system->realloc(buf, 0);

        /* Stores: the same 16 words over and over, in PSRAM vs on the stack */
        {
                volatile uint32_t *heap = pd->system->realloc(NULL, 64);
                volatile uint32_t stack[16];
                float t0 = pd->system->getElapsedTime();
                for (uint32_t i = 0; i < 400000; i++)
                        heap[i & 15] = i;
                float t1 = pd->system->getElapsedTime();
                for (uint32_t i = 0; i < 400000; i++)
                        stack[i & 15] = i;
                float t2 = pd->system->getElapsedTime();
                uint32_t acc = 0;
                for (uint32_t i = 0; i < 400000; i++)
                        acc += heap[i & 15];
                float t3 = pd->system->getElapsedTime();
                pd->system->logToConsole("bench: store hot heap %.1f ns, store stack %.1f ns, load hot heap %.1f ns (%u)",
                                         (double)((t1 - t0) * 1e9f / 400000), (double)((t2 - t1) * 1e9f / 400000),
                                         (double)((t3 - t2) * 1e9f / 400000), (unsigned)(acc & 1));
                pd->system->realloc((void *)heap, 0);
        }

        /* Pieces of the emulator's hot path, in isolation */
        {
                extern unsigned int cpu_read_word(unsigned int address);
                extern void (*const m68ki_static_instruction_jump_table[0x10000])(void);
                void (*nop)(void) = m68ki_static_instruction_jump_table[0x4e71];
                const int N = 100000;
                float t0 = pd->system->getElapsedTime();
                for (int i = 0; i < N; i++)
                        nop();
                float t1 = pd->system->getElapsedTime();
                unsigned acc = 0;
                for (int i = 0; i < N; i++)
                        acc += cpu_read_word(0x400000 + ((i & 63) << 1));
                float t2 = pd->system->getElapsedTime();
                for (int i = 0; i < N; i++)
                        acc += cpu_read_word(0x600000 + ((i & 63) << 1));
                float t3 = pd->system->getElapsedTime();
                pd->system->logToConsole("bench: nop handler %.1f ns, ROM read_word %.1f ns, RAM read_word %.1f ns (%u)",
                                         (double)((t1 - t0) * 1e9f / N), (double)((t2 - t1) * 1e9f / N),
                                         (double)((t3 - t2) * 1e9f / N), acc & 1);
        }

        /* Same thing over the jump table itself (read only, sequential-ish) */
        float t0 = pd->system->getElapsedTime();
        uintptr_t acc = 0;
        for (uint32_t i = 0, k = 0; i < 200000; i++, k = (k + 40503) & 0xffff)
                acc += (uintptr_t)m68ki_static_instruction_jump_table[k];
        float t1 = pd->system->getElapsedTime();
        pd->system->logToConsole("bench: random jumptable read %.1f ns (%u)", (double)((t1 - t0) * 1e9f / 200000), (unsigned)(acc & 1));
}
#endif
