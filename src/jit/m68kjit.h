/* 68000 -> Thumb-2 JIT, layered on Musashi.
 *
 * Basic blocks of common 68k instructions are translated to Thumb-2
 * functions that operate directly on Musashi's CPU state; anything else
 * is single-stepped by Musashi.  The two hand over at any instruction
 * boundary, so coverage can grow one opcode at a time.
 */
#ifndef M68KJIT_H
#define M68KJIT_H

#include <stddef.h>
#include <stdint.h>

enum {
        JH_RD8, JH_RD16, JH_RD32,       /* uint32 f(addr) */
        JH_WR8, JH_WR16, JH_WR32,       /* void f(addr, value) */
        JH_CODEWRITE,                   /* void f(addr, size): RAM store hit a code page */
        JH_ALINE,                       /* uint32 f(pc): take an A-line trap, returns new pc */
        JH_GETSR,                       /* uint32 f(): the status register */
        JH_SROP,                        /* uint32 f(pc, op << 16 | imm): ORI/ANDI/EORI to SR -> next pc */
        JH_MOVETOSR,                    /* uint32 f(pc, value): MOVE to SR -> next pc */
        JH_RTE,                         /* uint32 f(pc): RTE -> next pc */
        JH_INTERP,                      /* uint32 f(pc): one instruction via Musashi -> next pc */
        JH_MOVEM,                       /* void f(addr, spec): MOVEM (see tr_movem) */
        JH_NATIVE,                      /* uint32 f(index, pc): native ROM routine -> next pc */
        JH_COUNT
};

/* The JIT's own copy of the 68k registers.  Generated code works on this
 * (kept on the stack, which on the Playdate is fast internal SRAM) and it
 * is synced with Musashi's state at hand-over points.  Everything is
 * within 128 bytes so 16-bit loads/stores can reach it.
 */
typedef struct {
        uint32_t dar[16];       /* D0-D7, A0-A7 */
        uint32_t pc;
        uint32_t x, n, not_z, v, c;     /* Musashi flag formats */
        int32_t budget;         /* instructions left before returning */
        uint32_t lastexit;      /* chainable exit just taken (index + 1), or 0 */
        uint32_t native_n;      /* 68k instructions the last native routine stood in for */
} jregs_t;

typedef struct {
        /* Address of host memory as seen by generated code */
        uint32_t (*taddr)(const void *p);
        /* Address to call for helper n (with the Thumb bit) */
        uint32_t (*helper_addr)(int n);
        /* Call entry(code | 1, regs) */
        uint32_t (*run)(void *entry, void *code, jregs_t *regs);
        /* New code was written at [p, p+len) */
        void (*code_written)(void *p, uint32_t len);
        /* About to reuse code memory: make sure stale code can't run */
        void (*recycle)(void);
        void *(*alloc)(size_t size);
        /* Optional: seconds, for profiling */
        float (*now)(void);
        /* Optional: storage for the register file (default: the stack) */
        jregs_t *(*regs)(void);
} m68kjit_platform_t;

typedef struct {
        uint64_t blocks;        /* translated blocks executed */
        uint64_t jit_instrs;    /* 68k instructions executed in translated code */
        uint64_t interp_instrs; /* ... single-stepped by Musashi */
        uint32_t translations;
        uint32_t flushes;       /* code-write invalidations */
        uint32_t recycles;      /* code buffer wrapped */
        uint32_t conflicts;     /* lookup evicted another block */
        uint32_t stale;         /* block's page was invalidated */
        uint32_t smc_exits;     /* store sites that can leave a block early */
        uint32_t follows;       /* unconditional jumps folded into traces */
        uint32_t native_calls;  /* native ROM routines run */
        uint64_t native_instrs; /* ... and the 68k instructions they stood in for */
        uint64_t code_bytes;    /* generated */
        uint32_t chains;        /* exits linked straight to the next block */
        float t_run, t_xlat, t_interp, t_total;   /* seconds, if platform has now() */
} m68kjit_stats_t;

extern m68kjit_stats_t m68k_jit_stats;
extern int m68k_jit_enabled;

int     m68k_jit_init(const m68kjit_platform_t *plat, uint8_t *ram, uint32_t ram_size,
                      const uint8_t *rom, uint32_t rom_size, uint32_t code_size);
/* Drop-in for m68k_execute() */
int     m68k_jit_execute(int num_cycles);
/* Call on every RAM write made outside translated code */
void    m68k_jit_note_write(uint32_t addr, uint32_t size);
/* Testing: called (before the write) for every RAM write noted */
extern void (*m68k_jit_write_observer)(uint32_t addr, uint32_t size);
/* Testing: called with the address of each write that flushes translations */
extern void (*m68k_jit_flush_observer)(uint32_t addr);
/* Debugging: called after each translation */
extern void (*m68k_jit_translate_observer)(uint32_t pc, void *code, uint32_t len);
/* Helpers, for platforms that call them directly (by address) */
extern uint32_t (*const m68k_jit_helper_fn[JH_COUNT])(uint32_t a, uint32_t b);
/* Helpers, for platforms that can't call them directly */
uint32_t m68k_jit_helper(int n, uint32_t a, uint32_t b);
/* Testing: called for each instruction handed to the interpreter */
extern void (*m68k_jit_interp_observer)(uint32_t pc, uint32_t opcode);
/* Testing: copy the JIT register file to/from Musashi's state */
void    m68k_jit_sync_out(void);
void    m68k_jit_sync_in(void);
jregs_t *m68k_jit_regs(void);
/* Executes exactly one instruction with Musashi (no interrupt check) */
void    m68k_step_one(void);

#endif
