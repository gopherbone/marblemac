/* Encoder check: emit instructions, print "<hex>\t<asm>" lines; the
 * companion script assembles the asm with GNU as and compares.
 */
#include <stdio.h>
#include "../../src/jit/thumb.h"

static uint16_t buf[256];
static temit_t E;

#define T(asm, call) do { E.p = E.start = buf; E.end = buf + 256; E.full = 0; call; \
        printf("%s\t", asm); for (uint16_t *q = buf; q < E.p; q++) printf("%04x", *q); printf("\n"); } while (0)

int main(void)
{
        temit_t *e = &E;
        T("add.w r0, r1, r2", t_add(e, R0, R1, R2));
        T("adds.w r8, r10, r11", t_adds(e, R8, R10, R11));
        T("subs.w r0, r0, r1", t_subs(e, R0, R0, R1));
        T("ands.w r3, r4, r5", t_ands(e, R3, R4, R5));
        T("orrs.w r0, r1, r2", t_orrs(e, R0, R1, R2));
        T("eors.w r0, r1, r2", t_eors(e, R0, R1, R2));
        T("cmp.w r0, r12", t_cmp(e, R0, R12));
        T("tst.w r1, r2", t_tst(e, R1, R2));
        T("lsls.w r0, r1, #24", t_movsh(e, 1, R0, R1, SH_LSL, 24));
        T("lsrs.w r0, r1, #3", t_movsh(e, 1, R0, R1, SH_LSR, 3));
        T("asrs.w r0, r1, #31", t_movsh(e, 1, R0, R1, SH_ASR, 31));
        T("lsr.w r2, r1, #24", t_movsh(e, 0, R2, R1, SH_LSR, 24));
        T("mov r0, r8", t_mov(e, R0, R8));
        T("mov r10, r1", t_mov(e, R10, R1));
        T("movs r3, #200", t_movs8(e, R3, 200));
        T("ldr r0, [r4, #60]", t_ldr(e, R0, R4, 60));
        T("str r2, [r4, #8]", t_str(e, R2, R4, 8));
        T("ldr.w r2, [r4, #352]", t_ldr(e, R2, R4, 352));
        T("ldrh r0, [r5, #62]", t_ldst(e, LS_LDRH, R0, R5, 62));
        T("strb r1, [r6, #31]", t_ldst(e, LS_STRB, R1, R6, 31));
        T("ldr r0, [r5, r1]", t_ldstr(e, LS_LDR, R0, R5, R1));
        T("strh r2, [r5, r1]", t_ldstr(e, LS_STRH, R2, R5, R1));
        T("ldrb r2, [r6, r2]", t_ldstr(e, LS_LDRB, R2, R6, R2));
        T("rev r0, r0", t_rev(e, R0, R0));
        T("rev16 r2, r1", t_rev16(e, R2, R1));
        T("rev.w r2, r8", t_rev(e, R2, R8));
        T("bl .+0x1000", { uint16_t *a = e->p; t_bl_to(e, a + 0x800); });
        T("bl .-0x2004", { uint16_t *a = e->p; t_bl_to(e, a - 0x1002); });
        T("mvn.w r2, r1", t_mvn(e, R2, R1));
        T("adds.w r0, r0, r1, lsl #16", t_dpr(e, DP_ADD, 1, R0, R0, R1, SH_LSL, 16));
        T("add.w r0, r1, #1", t_addi(e, R0, R1, 1));
        T("sub.w r11, r11, #4", t_subi(e, R11, R11, 4));
        T("bic.w r1, r0, #0xff000000", t_bici(e, R1, R0, 0xff000000));
        T("tst.w r1, #0xc00000", t_tsti(e, R1, 0xc00000));
        T("and.w r2, r2, #0x80", t_andi(e, R2, R2, 0x80));
        T("eor.w r3, r2, #0x100", t_eori(e, R3, R2, 0x100));
        T("and.w r1, r1, #0x40000000", t_andi(e, R1, R1, 0x40000000));
        T("cmp.w r0, #0", t_cmpi(e, R0, 0));
        T("orr.w r0, r0, #0x00ff00ff", t_orri(e, R0, R0, 0x00ff00ff));
        T("orr.w r0, r0, #0xab00ab00", t_orri(e, R0, R0, 0xab00ab00));
        T("orr.w r0, r0, #0x7f7f7f7f", t_orri(e, R0, R0, 0x7f7f7f7f));
        T("rsbs.w r0, r0, #0", t_rsbsi(e, R0, R0, 0));
        T("adc.w r0, r0, r1", t_dpr(e, DP_ADC, 0, R0, R0, R1, SH_LSL, 0));
        T("sbcs.w r0, r0, r1", t_dpr(e, DP_SBC, 1, R0, R0, R1, SH_LSL, 0));
        T("lsls.w r0, r0, r1", t_shr(e, SH_LSL, 1, R0, R0, R1));
        T("ror.w r2, r3, r12", t_shr(e, SH_ROR, 0, R2, R3, R12));
        T("movw r0, #0x1234", t_movw(e, R0, 0x1234));
        T("movt r0, #0xabcd", t_movt(e, R0, 0xabcd));
        T("movw r12, #0xffff", t_movw(e, R12, 0xffff));
        T("ldr.w r0, [r4, #0x100]", t_ldr(e, R0, R4, 0x100));
        T("str.w r1, [r4, #0xfc]", t_str(e, R1, R4, 0xfc));
        T("ldrh.w r0, [r5, #0x42]", t_ldst(e, LS_LDRH, R0, R5, 0x42));
        T("strb.w r8, [r6, #0]", t_ldst(e, LS_STRB, R8, R6, 0));
        T("ldrsh.w r0, [r1, #6]", t_ldst(e, LS_LDRSH, R0, R1, 6));
        T("ldr.w r0, [r5, r12]", t_ldstr(e, LS_LDR, R0, R5, R12));
        T("ldrh.w r8, [r5, r1]", t_ldstr(e, LS_LDRH, R8, R5, R1));
        T("strb.w r8, [r5, r1]", t_ldstr(e, LS_STRB, R8, R5, R1));
        T("sxth.w r0, r1", t_sxth(e, R0, R1));
        T("uxth.w r0, r8", t_uxth(e, R0, R8));
        T("sxtb.w r2, r3", t_sxtb(e, R2, R3));
        T("uxtb.w r2, r3", t_uxtb(e, R2, R3));
        T("clz r2, r2", t_clz(e, R2, R2));
        T("mul r0, r0, r1", t_mul(e, R0, R0, R1));
        T("ubfx r0, r1, #8, #8", t_ubfx(e, R0, R1, 8, 8));
        T("sbfx r0, r1, #0, #16", t_sbfx(e, R0, R1, 0, 16));
        T("bfi r1, r0, #0, #8", t_bfi(e, R1, R0, 0, 8));
        T("bfi r1, r0, #0, #16", t_bfi(e, R1, R0, 0, 16));
        T("mrs r1, APSR", t_mrs_apsr(e, R1));
        T("msr APSR_nzcvq, r1", t_msr_apsr(e, R1));
        T("push.w {r4, r5, r6, r7, r8, r10, r11, lr}", t_push(e, 0x4DF0));
        T("pop.w {r4, r5, r6, r7, r8, r10, r11, pc}", t_pop(e, 0x8DF0));
        T("blx r12", t_blx(e, R12));
        T("bx lr", t_bx(e, LR));
        T("nop", t_nop(e));
        /* branches: forward +0x100 and backward -0x40 relative to the insn */
        T("b.w .+0x104", { tbr_t a = t_b_placeholder(e, C_AL); t_patch_branch(a, a.at + 0x82); });
        T("beq.w .+0x104", { tbr_t a = t_b_placeholder(e, C_EQ); t_patch_branch(a, a.at + 0x82); });
        T("bne.w .-0x3c", { tbr_t a = t_b_placeholder(e, C_NE); t_patch_branch(a, a.at - 0x1e); });
        T("b.w .-0x3c", { tbr_t a = t_b_placeholder(e, C_AL); t_patch_branch(a, a.at - 0x1e); });
        T("bhi.w .+0x40004", { tbr_t a = t_b_placeholder(e, C_HI); t_patch_branch(a, a.at + 0x20002); });
        T("b.w .+0x200004", { tbr_t a = t_b_placeholder(e, C_AL); t_patch_branch(a, a.at + 0x100002); });
        T("b.w .-0x1ffffc", { tbr_t a = t_b_placeholder(e, C_AL); t_patch_branch(a, a.at - 0xffffe); });
        /* an owed MRS comes out before a flag-setting instruction, not before a load */
        T("ldr r0, [r4, #60]; mrs r9, APSR; adds.w r0, r1, r2", { e->defer = R9 + 1; t_ldr(e, R0, R4, 60); t_adds(e, R0, R1, R2); });
        T("mrs r9, APSR; bl .+0x1c", { e->defer = R9 + 1; t_bl_to(e, e->p + 0x10); });
        /* mov32 picks the shortest form */
        T("mov.w r0, #0x80", t_mov32(e, R0, 0x80));
        T("mov.w r0, #0xffffffff", t_mov32(e, R0, 0xffffffff));
        T("mvn.w r0, #0x80", t_mov32(e, R0, 0xffffff7f));
        T("movw r0, #0x5678; movt r0, #0x1234", t_mov32(e, R0, 0x12345678));
        return 0;
}
