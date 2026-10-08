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
extern const int m68k_native_count;
/* Index into m68k_natives of the routine at pc, or -1 */
int     m68k_native_lookup(uint32_t pc);

/* Memory access for natives (size 2 or 4); writes check for translated code */
uint32_t m68k_jit_read(uint32_t addr, int size);
void    m68k_jit_write(uint32_t addr, uint32_t v, int size);

#endif
