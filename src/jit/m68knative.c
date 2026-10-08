/* Native replacements for hot Mac Plus ROM (v3) subroutines; see
 * m68knative.h.  Each comment gives the ROM code it replaces.  Flags use
 * Musashi's formats: n bit 7, not_z nonzero = Z clear, v bit 7, c bit 8.
 */

#include "m68knative.h"

#define D(n)    j->dar[n]
#define A(n)    j->dar[8 + (n)]

static uint32_t rd16(uint32_t a) { return m68k_jit_read(a & 0xffffff, 2); }
static uint32_t rd32(uint32_t a) { return m68k_jit_read(a & 0xffffff, 4); }

static void set_w(uint32_t *r, uint32_t v) { *r = (*r & 0xffff0000u) | (v & 0xffff); }

/* moveq #v,Dn's flags (X untouched) */
static void flags_moveq(jregs_t *j, int32_t v)
{
        j->n = (uint32_t)v >> 24;
        j->not_z = (uint32_t)v;
        j->v = 0;
        j->c = 0;
}

static uint32_t rts(jregs_t *j)
{
        uint32_t pc = rd32(A(7));
        A(7) += 4;
        return pc;
}

/* Resource Manager: find ID D2.w among D4.w+1 12-byte reference entries
 * at A2.  Found: A2 = the entry, D0 = 0; else D0 = -1.
 *
 *  413f10  moveq   #$c, D0
 *  413f12  cmp.w   (A2), D2
 *  413f14  adda.w  D0, A2
 *  413f16  dbeq    D4, $413f12
 *  413f1a  beq     $413f44
 *  413f1c  bra     $413f4a
 *  413f44  suba.w  D0, A2           413f4a  moveq   #-$1, D0
 *  413f46  moveq   #$0, D0          413f4c  rts
 *  413f48  rts
 */
static uint32_t find_ref_id(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t a2 = A(2), d2 = D(2) & 0xffff, d4 = D(4) & 0xffff, n = 1;
        int found = 0;
        for (;;) {
                uint32_t w = rd16(a2);
                a2 += 12;
                n += 3;
                if (w == d2) {
                        found = 1;
                        break;
                }
                d4 = (d4 - 1) & 0xffff;
                if (d4 == 0xffff)
                        break;
        }
        set_w(&D(4), d4);
        n += 4;                         /* beq; suba/moveq/rts or bra/moveq/rts */
        if (found)
                a2 -= 12;
        A(2) = a2;
        D(0) = found ? 0 : 0xffffffffu;
        flags_moveq(j, (int32_t)D(0));
        *ninstr = n;
        return rts(j);
}

/* Resource Manager: find type D3.l in the map whose handle is in A4.
 * Found: A2 = the type entry, D0 = 0; else D0 = -1.  A3 = map, D5 =
 * types left (as the dbeq left it).
 *
 *  413f1e  bsr     $413ee6     ->  413ee6  movea.l (A4), A3
 *  413f20  bmi     $413f4a             413ee8  adda.w  ($18,A3), A3
 *  413f22  moveq   #$8, D0             413eec  movea.l A3, A2
 *  413f24  cmp.l   (A2), D3            413eee  move.w  (A2)+, D5
 *  413f26  adda.w  D0, A2              413ef0  rts
 *  413f28  dbeq    D5, $413f24
 *  413f2c  beq     $413f44
 *  413f2e  bra     $413f4a         (413f44/413f4a as above)
 */
static uint32_t find_type(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        /* The bsr's return address lands below the stack */
        m68k_jit_write((A(7) - 4) & 0xffffff, pc + 2, 4);
        uint32_t a3 = rd32(A(4));
        a3 += (uint32_t)(int16_t)rd16(a3 + 0x18);
        uint32_t a2 = a3;
        uint32_t d5 = rd16(a2);
        a2 += 2;
        A(3) = a3;
        set_w(&D(5), d5);
        uint32_t n = 6 + 1;             /* bsr, 5 in the subroutine, bmi */
        if (d5 & 0x8000) {
                A(2) = a2;
                D(0) = 0xffffffffu;
                flags_moveq(j, -1);
                *ninstr = n + 2;        /* moveq, rts */
                return rts(j);
        }
        n += 1;                         /* moveq #8 */
        uint32_t d3 = D(3);
        int found = 0;
        for (;;) {
                uint32_t l = rd32(a2);
                a2 += 8;
                n += 3;
                if (l == d3) {
                        found = 1;
                        break;
                }
                d5 = (d5 - 1) & 0xffff;
                if (d5 == 0xffff)
                        break;
        }
        set_w(&D(5), d5);
        n += 4;
        if (found)
                a2 -= 8;
        A(2) = a2;
        D(0) = found ? 0 : 0xffffffffu;
        flags_moveq(j, (int32_t)D(0));
        *ninstr = n;
        return rts(j);
}

const m68k_native_t m68k_natives[] = {
        { 0x413f10, find_ref_id, "find resource ID" },
        { 0x413f1e, find_type, "find resource type" },
};
const int m68k_native_count = sizeof m68k_natives / sizeof m68k_natives[0];

int m68k_native_lookup(uint32_t pc)
{
        pc &= 0xffffff;
        if (pc < 0x400000)
                return -1;
        for (int i = 0; i < m68k_native_count; i++)
                if (m68k_natives[i].pc == pc)
                        return i;
        return -1;
}
