#!/bin/sh
# Builds the interpreter (src/jit/m68kinterp.c) for the Cortex-M7, the way
# the device build does, as an image jit_verify can run under Unicorn
# (SIMINTERP=OUTDIR): OUTDIR/interp_sim.bin (loaded at 0x0d000000) and
# OUTDIR/interp_sim.sym.  Its calls out (memory slow paths, helpers) go
# to stubs at 0x0e800000 + 4 * n, in the order of the list below, which
# jit_verify forwards to the real functions.
#
# usage: tools/jit/interp_sim.sh OUTDIR [extra cc flags]   (from the repo root)
set -e
OUT=$1; shift
ARMBIN="${ARM_TOOLCHAIN_BIN:-$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | tail -1)}"
[ -n "$ARMBIN" ] && PATH="$ARMBIN:$PATH"
mkdir -p "$OUT"
STUBS="cpu_read_byte cpu_read_word cpu_read_long cpu_write_word cpu_write_long m68k_jit_check_write
m68k_jit_get_sr m68k_jit_h_aline m68k_jit_h_div0 m68k_jit_h_fallback m68k_jit_h_movem m68k_jit_h_native
m68k_jit_h_rte m68k_jit_h_srop m68k_jit_h_super m68k_jit_h_wr8 m68k_jit_set_sr_then m68k_native_lookup"
DEFS=""
n=0
for s in $STUBS; do
	DEFS="$DEFS --defsym=$s=$(printf 0x%x $((0x0e800001 + 4 * n)))"
	n=$((n + 1))
done
arm-none-eabi-gcc -c -O2 -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 \
	-ffunction-sections -fdata-sections -Wall -Wno-unused-parameter \
	-Isrc -Isrc/jit -Iexternal/umac/include -Iexternal/umac/external/Musashi \
	-DMUSASHI_CNF="\"$PWD/external/umac/include/m68kconf.h\"" -DUMAC_MEMSIZE=4096 "$@" \
	-o "$OUT/interp_sim.o" src/jit/m68kinterp.c
cat > "$OUT/interp_sim.ld" <<'EOF'
SECTIONS {
	. = 0x0d000000;
	.text : { *(.text*) }
	.rodata : { *(.rodata*) }
	.data : { *(.data*) }
	.bss : { *(.bss*) *(COMMON) }
	/DISCARD/ : { *(.ARM.*) *(.comment) }
}
EOF
arm-none-eabi-ld -T "$OUT/interp_sim.ld" $DEFS -o "$OUT/interp_sim.elf" "$OUT/interp_sim.o" \
	"$(arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -mfloat-abi=hard -mfpu=fpv5-sp-d16 -print-libgcc-file-name)"
arm-none-eabi-objcopy -O binary -j .text -j .rodata -j .data "$OUT/interp_sim.elf" "$OUT/interp_sim.bin"
arm-none-eabi-nm "$OUT/interp_sim.elf" | awk '$3 == "m68k_interp_run" || $3 == "m68k_interp_env" { print $3, $1 }' > "$OUT/interp_sim.sym"
arm-none-eabi-nm -S --size-sort "$OUT/interp_sim.elf" | grep -i ' t ' > "$OUT/interp_sim.funcs"
cat "$OUT/interp_sim.sym"
