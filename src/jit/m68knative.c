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

/* SystemTask: walk the unit table for drivers that are open, not busy
 * and want periodic time.  Skips entries that don't qualify; at one that
 * does, stops just after its test (415dc2), leaving the ROM to run it.
 * Otherwise carries on after the loop (415de2).
 *
 *  415dac  move.l  (A3)+, D0
 *  415dae  beq     $415dde
 *  415db0  movea.l D0, A0
 *  415db2  movea.l (A0), A1
 *  415db4  move.w  ($4,A1), D0
 *  415db8  andi.w  #$20a0, D0
 *  415dbc  cmpi.w  #$2020, D0
 *  415dc0  bne     $415dde
 *  415dc2  ...                      (driver wants time)
 *  415dde  subq.w  #1, D3
 *  415de0  bgt     $415dac
 *  415de2  ...
 */
static uint32_t unit_scan(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        uint32_t a3 = A(3), d3 = D(3) & 0xffff, n = 0;
        for (;;) {
                uint32_t d0 = rd32(a3);
                a3 += 4;
                D(0) = d0;
                n += 2;                         /* move.l, beq */
                if (d0) {
                        uint32_t a1 = rd32(d0);
                        uint32_t fl = rd16(a1 + 4) & 0x20a0;
                        A(0) = d0;
                        A(1) = a1;
                        set_w(&D(0), fl);
                        n += 6;                 /* movea x2, move.w, andi, cmpi, bne */
                        if (fl == 0x2020) {
                                /* cmpi's flags: equal */
                                j->n = 0;
                                j->not_z = 0;
                                j->v = 0;
                                j->c = 0;
                                A(3) = a3;
                                set_w(&D(3), d3);
                                *ninstr = n;
                                return (pc & 0xff000000) | 0x415dc2;
                        }
                }
                /* subq.w #1, D3 (sets X too); bgt */
                uint32_t res = d3 - 1;
                j->x = j->c = (res >> 8) & 0x100;
                j->n = (res >> 8) & 0xff;
                j->not_z = res & 0xffff;
                j->v = ((1 ^ d3) & (res ^ d3)) >> 8 & 0x80;
                d3 = res & 0xffff;
                n += 2;
                int z = d3 == 0, neg = (d3 & 0x8000) != 0, v = j->v != 0;
                if (z || neg != v)
                        break;
        }
        A(3) = a3;
        set_w(&D(3), d3);
        *ninstr = n;
        return (pc & 0xff000000) | 0x415de2;
}

/* QuickDraw UnionRect(src1, src2, dst): Pascal, args on the stack.  The
 * Finder calls it twice per icon while working out window contents.
 *
 *  40a184  movea.l ($c,A7), A0         ; src1
 *  40a188  movea.l ($8,A7), A1         ; src2
 *  40a18c  move.w  (A0)+, D0           ; top = min, left = min,
 *  40a18e  cmp.w   (A1)+, D0           ; bottom = max, right = max:
 *  40a190  ble     $40a196             ; each move/cmp/bcc (+ move
 *  40a192  move.w  (-$2,A1), D0        ; from src2 if it wins)
 *  40a196  swap    D0
 *  ...     (left, then bottom/right into D1 with bge)
 *  40a1b8  movea.l ($4,A7), A0
 *  40a1bc  move.l  D0, (A0)+
 *  40a1be  move.l  D1, (A0)+
 *  40a1c0  bra     $40a1f0
 *  40a1f0  movea.l (A7)+, A0
 *  40a1f2  adda.w  #$c, A7
 *  40a1f6  jmp     (A0)
 */
static uint32_t union_rect(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t sp = A(7), s1 = rd32(sp + 12), s2 = rd32(sp + 8);
        uint32_t n = 2 + 2 + 7;         /* moveas, swaps, the tail */
        int16_t v[4];
        for (int i = 0; i < 4; i++) {
                int16_t a = (int16_t)rd16(s1 + 2 * i), b = (int16_t)rd16(s2 + 2 * i);
                int take_b = i < 2 ? a > b : a < b;     /* ble / bge not taken */
                v[i] = take_b ? b : a;
                n += 3 + take_b;
        }
        uint32_t d0 = (uint32_t)(uint16_t)v[0] << 16 | (uint16_t)v[1];
        uint32_t d1 = (uint32_t)(uint16_t)v[2] << 16 | (uint16_t)v[3];
        uint32_t dst = rd32(sp + 4);
        m68k_jit_write(dst & 0xffffff, d0, 4);
        m68k_jit_write((dst + 4) & 0xffffff, d1, 4);
        D(0) = d0;
        D(1) = d1;
        A(1) = s2 + 8;
        /* flags: move.l D1, (A0)+ */
        j->n = d1 >> 24;
        j->not_z = d1;
        j->v = j->c = 0;
        uint32_t ret = rd32(sp);
        A(0) = ret;
        A(7) = sp + 16;
        *ninstr = n;
        return ret;
}

const m68k_native_t m68k_natives[] = {
        { 0x413f10, find_ref_id, "find resource ID" },
        { 0x413f1e, find_type, "find resource type" },
        { 0x415dac, unit_scan, "SystemTask driver scan" },
        { 0x40a184, union_rect, "UnionRect" },
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
