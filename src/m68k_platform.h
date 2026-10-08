/* Force-included into the CPU core on the device build.  Code runs from
 * PSRAM through a 16KB I-cache, so pack the hot opcode handlers and memory
 * accessors together: the SDK linker script places plain .text first.
 */
#define M68K_FAST_FUNC(x)       __attribute__((section(".text"))) x
