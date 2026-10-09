/* Internal interface between the JIT (m68kjit.c) and the interpreter
 * (m68kinterp.c).  Both work on the same register file (jregs_t), so
 * either can take over at any instruction boundary.
 */
#ifndef M68KJIT_INT_H
#define M68KJIT_INT_H

#include <stdint.h>
#include "m68kjit.h"

/* Helpers in m68kjit.c (the same ones translated code calls).  Each takes
 * the instruction's own pc and returns the pc to carry on at.
 */
uint32_t m68k_jit_h_aline(uint32_t pc);           /* A-line trap; sets J->native_n */
uint32_t m68k_jit_h_native(uint32_t idx, uint32_t pc); /* native routine; sets J->native_n */
uint32_t m68k_jit_h_srop(uint32_t pc, uint32_t arg);   /* ORI/ANDI/EORI #imm,SR */
uint32_t m68k_jit_h_rte(uint32_t pc);
uint32_t m68k_jit_h_interp(uint32_t pc);          /* one instruction through Musashi */
uint32_t m68k_jit_h_fallback(uint32_t pc);        /* ... for the interpreter (counted, observed) */
uint32_t m68k_jit_h_div0(uint32_t pc);            /* DIVU/DIVS by zero: the exception, pc past the operand */
uint32_t m68k_jit_h_super(void);                  /* in supervisor mode? */
uint32_t m68k_jit_get_sr(void);
void    m68k_jit_h_movem(uint32_t ea, uint32_t spec);
uint32_t m68k_jit_set_sr_then(uint32_t sr, uint32_t next);
uint32_t m68k_jit_h_wr8(uint32_t addr, uint32_t v);    /* incl. the paravirtual disk */
/* A RAM write at offset addr hit a code page: retire translations it overwrote */
int     m68k_jit_check_write(uint32_t addr, uint32_t size);

/* MOVEM spec bits (h_movem): bits 0-15 register mask */
#define MV_LONG         (1u << 16)
#define MV_TOREGS       (1u << 17)
#define MV_AUTO         (1u << 18)      /* -(An) (store) / (An)+ (load) */

/* The interpreter.  Runs from J->pc, charging J->budget one per
 * instruction (plus what natives and the A-line dispatcher stand in for),
 * until the budget runs out or something needs the dispatcher.  Returns
 * why it stopped (IX_*).  Flags stay in Musashi's format in J throughout.
 */
enum {
        IX_BUDGET,      /* budget used up */
        IX_HOT,         /* reached hot code (J->pc): translate it? */
        IX_SYNC,        /* mode/SR change or Musashi-stepped instruction: check interrupts */
        IX_NOFETCH,     /* J->pc isn't in RAM or ROM: step it with Musashi */
};
int     m68k_interp_run(jregs_t *j);

/* Everything the interpreter needs besides the register file, in one
 * place (set up by m68kjit.c; the verifier also runs an ARM build of the
 * interpreter, with its own copy)
 */
typedef struct {
        uint8_t *ram;
        const uint8_t *rom;
        const uint8_t *codepage;        /* the JIT's map of pages holding translated code */
        uint8_t *heat;                  /* m68k_interp_heat */
        uint32_t ram_size, rom_mask;
        int32_t tier, hot;              /* m68k_jit_tier, m68k_jit_hot_threshold (refreshed per run) */
        uint8_t natbits[32];            /* pcs of native routines, hashed */
} m68k_interp_env_t;
extern m68k_interp_env_t m68k_interp_env;
void    m68k_interp_env_refresh(void);     /* (m68kjit.c) */

/* Tiering (see m68k_jit_execute) */
extern int m68k_jit_tier;               /* 0 interpret only, 1 always JIT, 2 JIT hot loops, 3 JIT hot code */
extern int m68k_jit_hot_threshold;      /* backward branches to a loop head before it's translated */
extern int m68k_jit_exit_heat;          /* heat each exit from translated code adds (0: none) */
extern int m68k_jit_heat_decay;         /* heat halves every this many instructions' time */
/* Per loop head (hashed): backward branches taken there lately */
#define HEAT_BITS       10
extern uint8_t *m68k_interp_heat;
static inline uint32_t m68k_heat_index(uint32_t pc)
{
        return ((pc >> 1) ^ (pc >> (HEAT_BITS + 1))) & ((1u << HEAT_BITS) - 1);
}

/* Testing (INTERP_OBSERVE builds): slow-path word/long accesses (I/O) made */
extern uint32_t m68k_interp_slow_io;

#endif
