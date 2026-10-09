/* Native replacements for hot Mac Plus ROM subroutines.
 *
 * When the JIT is asked for a block at one of these ROM addresses, it
 * calls the C version instead.  Each one does exactly what the 68k code
 * would (registers, flags, memory, including the return address its own
 * BSRs leave below the stack) and reports how many 68k instructions that
 * took, so emulated time and the lockstep checker see no difference.
 */
#ifndef M68KNATIVE_H
#define M68KNATIVE_H

#include <stdint.h>
#include "m68kjit.h"

/* Runs the routine at pc on the register file; returns the 68k pc to continue
 * at and sets *ninstr to the instructions the ROM code would have run.
 * Flags are left in Musashi format in j (the JIT flushes its own first).
 */
typedef uint32_t (*m68k_native_fn)(jregs_t *j, uint32_t pc, uint32_t *ninstr);

typedef struct {
        uint32_t pc;
        m68k_native_fn fn;
        const char *name;
} m68k_native_t;

extern const m68k_native_t m68k_natives[];
/* Per routine: times run and 68k instructions stood in for */
extern struct m68k_native_stat { uint32_t calls; uint64_t instrs; } m68k_native_stats[];
extern const int m68k_native_count;
/* All natives, not just the ones that pay on the device (and the native
 * A-line dispatch); default JIT_NATIVES_ALL (0) */
extern int m68k_jit_natives_all;
/* Index into m68k_natives of the routine at pc, or -1 */
int     m68k_native_lookup(uint32_t pc);

/* Memory access for natives (size 1, 2 or 4); writes check for translated code */
uint32_t m68k_jit_read(uint32_t addr, int size);
void    m68k_jit_write(uint32_t addr, uint32_t v, int size);
/* The status register (the register file's flags and the CPU's mode bits) */
uint32_t m68k_jit_get_sr(void);

#endif
