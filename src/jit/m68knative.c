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

static uint32_t rd8(uint32_t a) { return m68k_jit_read(a & 0xffffff, 1); }
static void wr8(uint32_t a, uint32_t v) { m68k_jit_write(a & 0xffffff, v, 1); }
static void wr16(uint32_t a, uint32_t v) { m68k_jit_write(a & 0xffffff, v, 2); }
static void wr32(uint32_t a, uint32_t v) { m68k_jit_write(a & 0xffffff, v, 4); }

/* Flags of move/tst/clr of size bytes (N, Z; V = C = 0; X untouched) */
static void flags_logic(jregs_t *j, uint32_t v, int size)
{
        int sh = 8 * size - 8;
        uint32_t m = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
        j->n = (v >> sh) & 0x80;
        j->not_z = v & m;
        j->v = 0;
        j->c = 0;
}

/* Flags of cmp src, dst (dst - src) of size bytes (X untouched) */
static void flags_cmp(jregs_t *j, uint32_t dst, uint32_t src, int size)
{
        int sh = 8 * size - 8;
        uint32_t m = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
        dst &= m;
        src &= m;
        uint32_t res = (dst - src) & m;
        j->n = (res >> sh) & 0x80;
        j->not_z = res;
        j->v = (((src ^ dst) & (res ^ dst)) >> sh) & 0x80;
        j->c = dst < src ? 0x100 : 0;
}

static void push32(jregs_t *j, uint32_t v)
{
        A(7) -= 4;
        wr32(A(7), v);
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

/* Resource Manager GetResource(type, id) / Get1Resource, from just after
 * the trap patch's jmp: StdEntry, find the current map, search the map
 * chain (FindType/GetRefList/FindID/NextMap), remember the hit, then the
 * resource-load vector ($7F0) for a RAM map whose resource is already
 * in memory or isn't to be loaded, and the common exit (StdExit).
 * Rare cases (RomMapInsert, CurMap bad, ROM map, a resource that needs
 * reading in, a patched load vector) go back to the ROM at that point.
 *
 *  413dee  bsr     $413f56             413e1a  bsr     $4140a8
 *  413df2  bsr     $413e9c             413e1e  bsr     $414130
 *  413df6  bpl     $413dfc             413e22  move.l  A0, ($e,A6)
 *  413dfc  move.w  (A0)+, D2           413e26  move.l  A0, $b84.w
 *  413dfe  move.l  (A0)+, D3           413e2a  bra     $413ffe
 *  413e00  clr.l   (A0)
 *  413e02  bsr     $413f1e   FindType
 *  413e06  bmi     $413e12
 *  413e08  bsr     $413f30   GetRefList
 *  413e0c  bsr     $413f10   FindID
 *  413e10  beq     $413e1a
 *  413e12  bsr     $413eca   NextMap
 *  413e16  beq     $413e02
 *  413e18  bra     $413e2a
 *
 *  413f56  clr.w   $a60.w              StdEntry
 *  413f5a  movea.l (A7)+, A0
 *  413f5c  link    A6, #-$32
 *  413f60  clr.w   (-$1c,A6)
 *  413f64  clr.w   (-$18,A6)
 *  413f68  move.b  $ba4.w, (-$17,A6)
 *  413f6e  movem.l D1-D7/A1-A4, -(A7)
 *  413f72  suba.l  A4, A4
 *  413f74  move.l  A0, -(A7)
 *  413f76  lea     ($8,A6), A0
 *  413f7a  tst.b   $b9e.w
 *  413f7e  beq     $413f84
 *  413f84  rts
 *
 *  413e9c  move.w  $a5a.w, D6          CurMap's map -> A4, A3, A2
 *  413ea0  tst.w   D6
 *  413ea2  bmi     $413f4a
 *  413ea6  bne     $413eac
 *  413ea8  move.w  $a58.w, D6
 *  413eac  movea.l $a50.w, A4
 *  413eb0  moveq   #$0, D0
 *  413eb2  bra     $413ebe
 *  413eb4  move.l  ($10,A3), D0
 *  413eb8  beq     $413f4a
 *  413ebc  exg     D0, A4
 *  413ebe  movea.l (A4), A3
 *  413ec0  cmp.w   ($14,A3), D6
 *  413ec4  bne     $413eb4
 *  413ec6  movea.l A3, A2
 *  413ec8  rts
 *
 *  413f30  move.w  ($4,A2), D4         GetRefList
 *  413f34  move.l  D0, -(A7)
 *  413f36  moveq   #$0, D0
 *  413f38  move.w  ($6,A2), D0
 *  413f3c  movea.l D0, A2
 *  413f3e  move.l  (A7)+, D0
 *  413f40  adda.l  A3, A2
 *  413f42  rts
 *
 *  413eca  tst.b   $b9a.w              NextMap
 *  413ece  beq     $413ed4
 *  413ed0  suba.l  A4, A4
 *  413ed2  bra     $413eda
 *  413ed4  movea.l (A4), A3
 *  413ed6  movea.l ($10,A3), A4
 *  413eda  move.l  A4, D0
 *  413edc  beq     $413f4a
 *  413ede  movea.l (A4), A3
 *  413ee0  move.w  ($14,A3), D6
 *  413ee4  bra     $413f46
 *  413f46  moveq #0, D0; rts           413f4a  moveq #-1, D0; rts
 *
 *  4140a8  move.w  $a06.w, $b9c.w      remember the hit
 *  4140ae  move.l  (A4), D0
 *  4140b0  suba.l  D0, A2
 *  4140b2  suba.l  D0, A3
 *  4140b4  movem.l D3/A1-A4, $b80.w
 *  4140ba  movem.w D4-D6, $b94.w
 *  4140c0  adda.l  D0, A2
 *  4140c2  adda.l  D0, A3
 *  4140c4  rts
 *
 *  414130  movea.l $7f0.w, A0          load vector
 *  414134  jmp     (A0)
 *  414136  move.b  ($4,A2), D1         (the ROM's load routine)
 *  41413a  cmpa.l  $b06.w, A4
 *  41413e  beq     $414102
 *  414140  move.l  $118.w, -(A7)
 *  414144  btst    #$6, D1
 *  414148  beq     $414150
 *  41414a  move.l  $2a6.w, $118.w
 *  414150  movea.l ($8,A2), A0
 *  414154  suba.l  (A4), A3
 *  414156  tst.b   $a5e.w
 *  41415a  beq     $41419e
 *  41415c  move.l  A0, D0
 *  41415e  bne     $41417a
 *  41417a  tst.l   (A0)
 *  41417c  bne     $4141a8
 *  41419e  move.l  A0, D0
 *  4141a0  bne     $4141a8
 *  4141a8  move.l  A0, D0
 *  4141aa  beq     $4141b0
 *  4141ac  move.l  A0, ($8,A2)
 *  4141b0  adda.l  (A4), A3
 *  4141b2  move.l  (A7)+, $118.w
 *  4141b6  rts
 *
 *  413ffe  moveq   #$6, D0             StdExit
 *  414000  bra     $41400c
 *  41400c  tst.b   $b9e.w
 *  414010  beq     $414018
 *  414018  clr.b   $b9a.w
 *  41401c  clr.w   $b9e.w
 *  414020  clr.b   $ba4.w
 *  414024  movem.l (A7)+, D1-D7/A1-A4
 *  414028  unlk    A6
 *  41402a  movea.l (A7)+, A0
 *  41402c  adda.l  D0, A7
 *  41402e  move.l  A0, -(A7)
 *  414030  suba.l  A0, A0
 *  414032  move.w  $a60.w, D0
 *  414036  beq     $414040
 *  414038  move.l  $af2.w, -(A7)
 *  41403c  bne     $414040
 *  41403e  addq.w  #4, A7
 *  414040  rts
 */
static uint32_t get_resource(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t n, sub, b;
        /* StdEntry */
        push32(j, 0x413df2);                            /* bsr */
        wr16(0xa60, 0);
        A(0) = rd32(A(7));
        A(7) += 4;
        push32(j, A(6));                                /* link */
        A(6) = A(7);
        A(7) -= 0x32;
        wr16(A(6) - 0x1c, 0);
        wr16(A(6) - 0x18, 0);
        b = rd8(0xba4);
        wr8(A(6) - 0x17, b);
        for (int r = 12; r >= 1; r--)                   /* movem D1-D7/A1-A4 */
                if (r != 8)
                        push32(j, j->dar[r]);
        A(4) = 0;
        push32(j, A(0));
        A(0) = A(6) + 8;
        b = rd8(0xb9e);
        flags_logic(j, b, 1);
        n = 13;
        if (b) {
                *ninstr = n;
                return 0x413f80;
        }
        A(7) += 4;                                      /* rts */
        n++;

        /* Find the current map */
        push32(j, 0x413df6);
        uint32_t d6 = rd16(0xa5a);
        set_w(&D(6), d6);
        flags_logic(j, d6, 2);
        n += 4;                                         /* bsr, move, tst, bmi */
        if (d6 & 0x8000) {
                *ninstr = n;
                return 0x413f4a;
        }
        n++;                                            /* bne */
        if (!d6) {
                d6 = rd16(0xa58);
                set_w(&D(6), d6);
                n++;
        }
        A(4) = rd32(0xa50);
        D(0) = 0;
        n += 3;                                         /* movea, moveq, bra */
        for (;;) {
                A(3) = rd32(A(4));
                uint32_t w = rd16(A(3) + 0x14);
                flags_cmp(j, d6, w, 2);
                n += 3;                                 /* movea, cmp, bne */
                if (w == d6)
                        break;
                D(0) = rd32(A(3) + 0x10);
                flags_logic(j, D(0), 4);
                n += 2;                                 /* move, beq */
                if (!D(0)) {
                        *ninstr = n;
                        return 0x413f4a;
                }
                uint32_t t = D(0);
                D(0) = A(4);
                A(4) = t;
                n++;                                    /* exg */
        }
        A(2) = A(3);
        A(7) += 4;
        n += 3;                                         /* movea, rts, bpl */

        /* Arguments */
        set_w(&D(2), rd16(A(0)));
        A(0) += 2;
        D(3) = rd32(A(0));
        A(0) += 4;
        wr32(A(0), 0);
        flags_logic(j, 0, 4);
        n += 3;

        /* Search the map chain */
        int found;
        for (;;) {
                push32(j, 0x413e06);
                find_type(j, 0x413f1e, &sub);
                n += 2 + sub;                           /* bsr, ..., bmi */
                if (!(D(0) & 0x80000000u)) {
                        push32(j, 0x413e0c);            /* bsr GetRefList */
                        uint32_t a2 = A(2);
                        set_w(&D(4), rd16(a2 + 4));
                        push32(j, D(0));
                        D(0) = rd16(a2 + 6);
                        A(2) = D(0);
                        D(0) = rd32(A(7));
                        A(7) += 4;
                        flags_logic(j, D(0), 4);
                        A(2) += A(3);
                        A(7) += 4;                      /* rts */
                        n += 9;
                        push32(j, 0x413e10);
                        find_ref_id(j, 0x413f10, &sub);
                        n += 2 + sub;                   /* bsr, ..., beq */
                        if (!D(0)) {
                                found = 1;
                                break;
                        }
                }
                /* NextMap */
                push32(j, 0x413e16);
                b = rd8(0xb9a);
                if (b)
                        A(4) = 0;
                else {
                        A(3) = rd32(A(4));
                        A(4) = rd32(A(3) + 0x10);
                }
                D(0) = A(4);
                flags_logic(j, D(0), 4);
                n += 7;                                 /* bsr, tst, beq, 2, move, beq */
                if (!A(4)) {
                        D(0) = 0xffffffffu;
                        flags_moveq(j, -1);
                        A(7) += 4;
                        n += 4;                         /* moveq, rts, beq, bra */
                        found = 0;
                        break;
                }
                A(3) = rd32(A(4));
                d6 = rd16(A(3) + 0x14);
                set_w(&D(6), d6);
                D(0) = 0;
                flags_moveq(j, 0);
                A(7) += 4;
                n += 6;                                 /* movea, move, bra, moveq, rts, beq */
        }

        if (found) {
                /* Remember it */
                push32(j, 0x413e1e);
                wr16(0xb9c, rd16(0xa06));
                D(0) = rd32(A(4));
                A(2) -= D(0);
                A(3) -= D(0);
                wr32(0xb80, D(3));
                wr32(0xb84, A(1));
                wr32(0xb88, A(2));
                wr32(0xb8c, A(3));
                wr32(0xb90, A(4));
                wr16(0xb94, D(4));
                wr16(0xb96, D(5));
                wr16(0xb98, D(6));
                flags_logic(j, D(0), 4);
                A(2) += D(0);
                A(3) += D(0);
                A(7) += 4;
                n += 10;                                /* bsr + 9 */

                /* The load vector */
                push32(j, 0x413e22);
                A(0) = rd32(0x7f0);
                n += 3;                                 /* bsr, movea, jmp */
                if ((A(0) & 0xffffff) != 0x414136) {
                        *ninstr = n;
                        return A(0);
                }
                b = rd8(A(2) + 4);
                D(1) = (D(1) & 0xffffff00u) | b;
                uint32_t rm = rd32(0xb06);
                flags_cmp(j, A(4), rm, 4);
                n += 3;
                if (A(4) == rm) {
                        *ninstr = n;
                        return 0x414102;
                }
                uint32_t save118 = rd32(0x118);
                push32(j, save118);
                flags_logic(j, save118, 4);
                j->not_z = b & 0x40;                    /* btst */
                n += 3;
                if (b & 0x40) {
                        uint32_t v = rd32(0x2a6);
                        wr32(0x118, v);
                        flags_logic(j, v, 4);
                        n++;
                }
                A(0) = rd32(A(2) + 8);
                A(3) -= rd32(A(4));
                b = rd8(0xa5e);
                n += 4;                                 /* movea, suba, tst, beq */
                D(0) = A(0);
                flags_logic(j, D(0), 4);
                n += 2;                                 /* move, bne */
                if (b) {
                        if (!A(0)) {
                                *ninstr = n;
                                return 0x414160;
                        }
                        flags_logic(j, rd32(A(0)), 4);
                        n += 2;                         /* tst, bne */
                        if (!j->not_z) {
                                *ninstr = n;
                                return 0x41417e;
                        }
                } else if (!A(0)) {
                        *ninstr = n;
                        return 0x4141a2;
                }
                D(0) = A(0);
                wr32(A(2) + 8, A(0));
                A(3) += rd32(A(4));
                uint32_t v = rd32(A(7));
                A(7) += 4;
                wr32(0x118, v);
                A(7) += 4;                              /* rts */
                n += 6;

                wr32(A(6) + 0xe, A(0));
                wr32(0xb84, A(0));
                flags_logic(j, A(0), 4);
                n += 3;                                 /* move, move, bra */
        } else
                n++;                                    /* bra $413ffe */

        /* StdExit */
        D(0) = 6;
        b = rd8(0xb9e);
        flags_logic(j, b, 1);
        n += 4;
        if (b) {
                *ninstr = n;
                return 0x414012;
        }
        wr8(0xb9a, 0);
        wr16(0xb9e, 0);
        wr8(0xba4, 0);
        for (int r = 1; r <= 12; r++)
                if (r != 8) {
                        j->dar[r] = rd32(A(7));
                        A(7) += 4;
                }
        A(7) = A(6);                                    /* unlk */
        A(6) = rd32(A(7));
        A(7) += 4;
        uint32_t ret = rd32(A(7));
        A(7) += 4 + 6;
        push32(j, ret);
        A(0) = 0;
        uint32_t err = rd16(0xa60);
        D(0) = err;
        flags_logic(j, err, 2);
        n += 11;                                        /* clr x3 .. beq */
        if (err) {
                uint32_t proc = rd32(0xaf2);
                push32(j, proc);
                flags_logic(j, proc, 4);
                n += 2;
                if (!proc) {
                        A(7) += 4;
                        n++;
                }
        }
        *ninstr = n + 1;                                /* rts */
        return rts(j);
}

/* SetCursor's body: copy the 16+16 longs of image and mask to TheCrsr
 * ($844), clamp the hot spot to 0..16 and store it at $886 ($884 long),
 * counting changes in D1.  If anything changed, carry on in the ROM
 * (hide and show the cursor); otherwise return.  The Finder calls it
 * with the same cursor every time round its loop.
 *
 *  401e0a  movea.l ($8,A7), A0         401e24  move.l  ($e,A7), D0
 *  401e0e  lea     $844.w, A1          401e28  moveq   #$10, D2
 *  401e12  moveq   #$f, D2             401e2a  cmp.w   D2, D0
 *  401e14  moveq   #$0, D1             401e2c  bls     $401e30
 *  401e16  move.l  (A0)+, D0           401e2e  move.w  D2, D0
 *  401e18  cmp.l   (A1), D0            401e30  swap    D0
 *  401e1a  beq     $401e1e             401e32  cmp.w   D2, D0
 *  401e1c  addq.w  #1, D1              401e34  bls     $401e38
 *  401e1e  move.l  D0, (A1)+           401e36  move.w  D2, D0
 *  401e20  dbra    D2, $401e16         401e38  swap    D0
 *                                      401e3a  cmp.l   $884.w, D0
 *                                      401e3e  beq     $401e46
 *                                      401e40  addq.w  #1, D1
 *                                      401e42  move.l  D0, $884.w
 *                                      401e46  tst.w   D1
 *                                      401e48  beq     $401e52
 *                                      401e4a  ...     (hide/show)
 *                                      401e52  movea.l (A7)+, A0
 *                                      401e54  adda.w  #$e, A7
 *                                      401e58  jmp     (A0)
 */
static uint32_t set_cursor(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t a0 = rd32(A(7) + 8), a1 = 0x844, d0, d1 = 0, n = 4;
        for (int i = 0; i < 16; i++) {
                d0 = rd32(a0);
                a0 += 4;
                if (d0 != rd32(a1)) {
                        d1++;
                        n++;
                }
                wr32(a1, d0);
                a1 += 4;
                n += 5;
        }
        d0 = rd32(A(7) + 0xe);
        if ((d0 & 0xffff) > 0x10) {
                d0 = (d0 & 0xffff0000u) | 0x10;
                n++;
        }
        d0 = d0 << 16 | d0 >> 16;
        if ((d0 & 0xffff) > 0x10) {
                d0 = (d0 & 0xffff0000u) | 0x10;
                n++;
        }
        d0 = d0 << 16 | d0 >> 16;
        n += 10;
        if (d0 != rd32(0x884)) {
                d1++;
                wr32(0x884, d0);
                n += 2;
        }
        n += 2;                                 /* tst, beq */
        if (d1)
                j->x = 0;                       /* addq.w #1, D1 (from 0..17) */
        flags_logic(j, d1, 2);
        D(0) = d0;
        D(1) = d1;
        D(2) = 0x10;                            /* moveq #$10 */
        A(0) = a0;
        A(1) = a1;
        if (d1) {
                *ninstr = n;
                return 0x401e4a;
        }
        uint32_t ret = rd32(A(7));
        A(0) = ret;
        A(7) += 4 + 0xe;
        *ninstr = n + 3;
        return ret;
}

/* Flags of add (dst + src) of size bytes, X too */
static void flags_add(jregs_t *j, uint32_t dst, uint32_t src, int size)
{
        int sh = 8 * size - 8;
        uint32_t m = size == 4 ? 0xffffffffu : (1u << (8 * size)) - 1;
        dst &= m;
        src &= m;
        uint32_t res = (dst + src) & m;
        j->n = (res >> sh) & 0x80;
        j->not_z = res;
        j->v = (((src ^ res) & (dst ^ res)) >> sh) & 0x80;
        j->x = j->c = res < dst ? 0x100 : 0;
}

/* Loops can't run away: past this many instructions, stop at the loop
 * head and let the ROM (and interrupts) have a go.
 */
#define LOOP_CAP 20000

/* QuickDraw: intersection of D0.w rects (SectRect and friends), Pascal
 * frame (count.w at 12(A6), then pointers), result rect at 8(A6).
 *
 *  40a110  link    A6, #$0             40a14c  cmp.w   D1, D3
 *  40a114  movem.l D1-D4/A1, -(A7)     40a14e  ble     $40a15a
 *  40a118  lea     ($c,A6), A1         40a150  cmp.w   D2, D4
 *  40a11c  move.w  (A1)+, D0           40a152  ble     $40a15a
 *  40a11e  ble     $40a15a             40a154  dbra    D0, $40a12a
 *  40a120  movea.l (A1)+, A0           40a158  bra     $40a162
 *  40a122  movem.w (A0)+, D1-D4        40a15a  clr.w   D1 (.. D4)
 *  40a126  subq.w  #1, D0              40a162  movea.l ($8,A6), A0
 *  40a128  bra     $40a14c             40a166  move.w  D1, (A0)+ (.. D4)
 *  40a12a  movea.l (A1)+, A0           40a16e  move.w  ($c,A6), D0
 *  40a12c  cmp.w   (A0)+, D1           40a172  lsl.w   #2, D0
 *  40a12e  bge     $40a134             40a174  addq.w  #6, D0
 *  40a130  move.w  (-$2,A0), D1        40a176  cmp.w   D1, D3
 *  ...     (D2 bge, D3 ble, D4 ble)    40a178  movem.l (A7)+, D1-D4/A1
 *                                      40a17c  unlk    A6
 *                                      40a17e  movea.l (A7)+, A0
 *                                      40a180  adda.w  D0, A7
 *                                      40a182  jmp     (A0)
 */
static uint32_t sect_rects(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        push32(j, A(6));
        A(6) = A(7);
        static const int save[5] = { 9, 4, 3, 2, 1 };   /* A1, D4..D1 */
        for (int i = 0; i < 5; i++)
                push32(j, j->dar[save[i]]);
        uint32_t a1 = A(6) + 0xc, a0;
        uint32_t cnt = rd16(a1);
        a1 += 2;
        uint32_t n = 5;
        int16_t r[4];
        if ((int16_t)cnt <= 0) {
                r[0] = r[1] = r[2] = r[3] = 0;
                n += 4;
        } else {
                a0 = rd32(a1);
                a1 += 4;
                for (int i = 0; i < 4; i++)
                        r[i] = (int16_t)rd16(a0 + 2 * i);
                a0 += 8;
                cnt = (cnt - 1) & 0xffff;
                n += 4;                                 /* movea, movem, subq, bra */
                for (;;) {
                        n += 2;
                        if (r[2] <= r[0])
                                goto empty;
                        n += 2;
                        if (r[3] <= r[1])
                                goto empty;
                        n++;                            /* dbra */
                        cnt = (cnt - 1) & 0xffff;
                        if (cnt == 0xffff) {
                                n++;                    /* bra */
                                break;
                        }
                        a0 = rd32(a1);
                        a1 += 4;
                        n += 1 + 8;
                        for (int i = 0; i < 4; i++) {
                                int16_t v = (int16_t)rd16(a0 + 2 * i);
                                if (i < 2 ? r[i] < v : r[i] > v) {
                                        r[i] = v;
                                        n++;
                                }
                        }
                        a0 += 8;
                        continue;
empty:
                        r[0] = r[1] = r[2] = r[3] = 0;
                        n += 4;
                        break;
                }
        }
        uint32_t dst = rd32(A(6) + 8);
        for (int i = 0; i < 4; i++)
                wr16(dst + 2 * i, (uint16_t)r[i]);
        uint32_t c = rd16(A(6) + 0xc);
        uint32_t sh = (c << 2) & 0xffff;
        uint32_t d0 = (sh + 6) & 0xffff;
        D(0) = (D(0) & 0xffff0000u) | d0;
        j->x = d0 < 6 ? 0x100 : 0;                      /* addq.w's carry */
        flags_cmp(j, (uint16_t)r[2], (uint16_t)r[0], 2);
        for (int i = 4; i >= 0; i--)                    /* D1..D4, A1 */
                j->dar[save[i]] = rd32(A(7) + 4 * (4 - i));
        A(7) = A(6);
        A(6) = rd32(A(7));
        uint32_t ret = rd32(A(7) + 4);
        A(0) = ret;
        A(7) += 8 + (uint32_t)(int16_t)d0;
        *ninstr = n + 14;       /* movea, 4 moves, move/lsl/addq/cmp, movem, unlk, movea, adda, jmp */
        return ret;
}

/* Resource Manager: scan D4.w+1 12-byte reference entries at A2 for the
 * biggest data offset (masked by $31A) that is below D2 and above D7;
 * keeps it in D7 and its entry in D6.
 *
 *  4138ca  move.l  ($4,A2), D0
 *  4138ce  and.l   $31a.w, D0
 *  4138d2  cmp.l   D0, D2
 *  4138d4  bge     $4138de
 *  4138d6  cmp.l   D0, D7
 *  4138d8  ble     $4138de
 *  4138da  move.l  D0, D7
 *  4138dc  move.l  A2, D6
 *  4138de  adda.w  #$c, A2
 *  4138e2  dbra    D4, $4138ca
 */
static uint32_t ref_max_below(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t mask = rd32(0x31a), n = 0, d4 = D(4) & 0xffff;
        for (;;) {
                uint32_t d0 = rd32(A(2) + 4) & mask;
                D(0) = d0;
                flags_cmp(j, D(2), d0, 4);
                n += 4;
                if ((int32_t)D(2) < (int32_t)d0) {
                        flags_cmp(j, D(7), d0, 4);
                        n += 2;
                        if ((int32_t)D(7) > (int32_t)d0) {
                                D(7) = d0;
                                D(6) = A(2);
                                flags_logic(j, D(6), 4);
                                n += 2;
                        }
                }
                A(2) += 0xc;
                n += 2;
                d4 = (d4 - 1) & 0xffff;
                if (d4 == 0xffff) {
                        set_w(&D(4), d4);
                        *ninstr = n;
                        return 0x4138e6;
                }
                if (n >= LOOP_CAP) {
                        set_w(&D(4), d4);
                        *ninstr = n;
                        return 0x4138ca;
                }
        }
}

/* Resource Manager: find handle A1 among all the references of a map
 * (type entries at A0, refs at A2): what AddResource and friends use to
 * check a handle isn't a resource already.
 *
 *  41405e  addq.w  #2, A0
 *  414060  addq.w  #8, A2
 *  414062  cmpa.l  (A2)+, A1
 *  414064  dbeq    D4, $414060
 *  414068  beq     $41407e
 *  41406a  move.l  (A0)+, D3
 *  41406c  move.w  (A0)+, D4
 *  41406e  dbra    D5, $41405e
 *  414072  ...                       (not in this map)
 */
static uint32_t ref_find_handle(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t n = 0, a0 = A(0), a2 = A(2), a1 = A(1), d4 = D(4) & 0xffff, d5 = D(5) & 0xffff;
        for (;;) {
                a0 += 2;
                n++;
                int eq;
                for (;;) {
                        a2 += 8;
                        uint32_t m = rd32(a2);
                        a2 += 4;
                        n += 3;
                        flags_cmp(j, a1, m, 4);
                        eq = m == a1;
                        if (eq)
                                break;
                        d4 = (d4 - 1) & 0xffff;
                        if (d4 == 0xffff)
                                break;
                }
                n++;                                    /* beq */
                if (eq) {
                        A(0) = a0;
                        A(2) = a2;
                        set_w(&D(4), d4);
                        set_w(&D(5), d5);
                        *ninstr = n;
                        return 0x41407e;
                }
                D(3) = rd32(a0);
                d4 = rd16(a0 + 4);
                a0 += 6;
                flags_logic(j, d4, 2);
                n += 3;
                d5 = (d5 - 1) & 0xffff;
                if (d5 == 0xffff || n >= LOOP_CAP) {
                        A(0) = a0;
                        A(2) = a2;
                        set_w(&D(4), d4);
                        set_w(&D(5), d5);
                        *ninstr = n;
                        return d5 == 0xffff ? 0x414072 : 0x41405e;
                }
        }
}

/* Memory Manager: walk the heap zone at A6 from block A1 for a free block
 * of at least D0 bytes (D2 = the address mask), merging each free block
 * with the free blocks after it.  Leaves at 4106ec when it reaches the
 * zone's end (bkLim at (A6)), at 4106e8 with the block in A1.
 *
 *  4106c2  cmpa.l  (A6), A1            4106d2  movea.l A1, A0
 *  4106c4  bcc     $4106ec             4106d4  adda.l  D1, A0
 *  4106c6  move.l  (A1), D1            4106d6  tst.b   (A0)
 *  4106c8  tst.b   (A1)                4106d8  bne     $4106e4
 *  4106ca  beq     $4106d2             4106da  cmpa.l  (A6), A0
 *  4106cc  and.l   D2, D1              4106dc  bcc     $4106e4
 *  4106ce  adda.l  D1, A1              4106de  add.l   (A0), D1
 *  4106d0  bra     $4106c2             4106e0  move.l  D1, (A1)
 *                                      4106e2  bra     $4106d2
 *                                      4106e4  cmp.l   D0, D1
 *                                      4106e6  bcs     $4106cc
 */
static uint32_t heap_find_free(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t n = 0, a1 = A(1), d1 = D(1);
        for (;;) {
                uint32_t lim = rd32(A(6));
                flags_cmp(j, a1, lim, 4);
                n += 2;
                if (a1 >= lim) {
                        A(1) = a1;
                        D(1) = d1;
                        *ninstr = n;
                        return 0x4106ec;
                }
                d1 = rd32(a1);
                uint32_t tag = rd8(a1);
                flags_logic(j, tag, 1);
                n += 3;
                if (!tag) {
                        /* free: merge the free blocks after it */
                        for (;;) {
                                uint32_t a0 = a1 + d1, t2 = rd8(a0);
                                A(0) = a0;
                                flags_logic(j, t2, 1);
                                n += 4;
                                if (t2)
                                        break;
                                uint32_t lim2 = rd32(A(6));
                                flags_cmp(j, a0, lim2, 4);
                                n += 2;
                                if (a0 >= lim2)
                                        break;
                                uint32_t s = rd32(a0);
                                flags_add(j, d1, s, 4);
                                d1 += s;
                                wr32(a1, d1);
                                {
                                        uint32_t x = j->x;
                                        flags_logic(j, d1, 4);
                                        j->x = x;
                                }
                                n += 3;
                                if (n >= LOOP_CAP) {
                                        A(1) = a1;
                                        D(1) = d1;
                                        *ninstr = n;
                                        return 0x4106d2;
                                }
                        }
                        flags_cmp(j, d1, D(0), 4);
                        n += 2;
                        if (d1 >= D(0)) {
                                A(1) = a1;
                                D(1) = d1;
                                *ninstr = n;
                                return 0x4106e8;
                        }
                }
                d1 &= D(2);
                flags_logic(j, d1, 4);
                a1 += d1;
                n += 3;
                if (n >= LOOP_CAP) {
                        A(1) = a1;
                        D(1) = d1;
                        *ninstr = n;
                        return 0x4106c2;
                }
        }
}

/* Window Manager FrontWindow: the first visible window in WindowList
 * ($9D6) that isn't the ghost window ($A84); nil while $8F2 is set.
 *
 *  411d72  clr.l   ($4,A7)             411d8a  tst.b   ($6e,A0)
 *  411d76  tst.b   $8f2.w              411d8e  bne     $411d96
 *  411d7a  bne     $411d9a             411d90  movea.l ($90,A0), A0
 *  411d7c  movea.l $9d6.w, A0          411d94  bra     $411d80
 *  411d80  move.l  A0, D0              411d96  move.l  A0, ($4,A7)
 *  411d82  beq     $411d96             411d9a  rts
 *  411d84  cmpa.l  $a84.w, A0
 *  411d88  beq     $411d90
 */
static uint32_t front_window(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        (void)pc;
        uint32_t n = 3;
        wr32(A(7) + 4, 0);
        uint32_t b = rd8(0x8f2);
        flags_logic(j, b, 1);
        if (!b) {
                uint32_t a0 = rd32(0x9d6), ghost = rd32(0xa84);
                n++;
                for (;;) {
                        D(0) = a0;
                        flags_logic(j, a0, 4);
                        n += 2;
                        if (!a0)
                                break;
                        flags_cmp(j, a0, ghost, 4);
                        n += 2;
                        if (a0 != ghost) {
                                uint32_t v = rd8(a0 + 0x6e);
                                flags_logic(j, v, 1);
                                n += 2;
                                if (v)
                                        break;
                        }
                        a0 = rd32(a0 + 0x90);
                        n += 2;
                        if (n >= LOOP_CAP) {
                                A(0) = a0;
                                *ninstr = n;
                                return 0x411d80;
                        }
                }
                A(0) = a0;
                wr32(A(7) + 4, a0);
                flags_logic(j, a0, 4);
                n++;
        }
        *ninstr = n + 1;
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

/* QuickDraw bitblt ($40a284): the row loop.  By $40a41e everything is set
 * up: A4/A5 = source/destination word, A0 = +-2 (direction), D4/D5 =
 * left/right edge masks, D6 = source shift (pattern modes: the pattern
 * row), D7 = invert mask (pattern modes: pattern row index * 4), D3.w =
 * rows - 1, -$4e(A6) = words per row - 1, -$4a(A6)/A3 = source/destination
 * row bumps, and A1 = the transfer loop for the mode (picked from the
 * table at $40a450).  The native runs whole rows of every loop the table
 * names and hands back at the row head after about BLT_CAP instructions
 * (so interrupts aren't held off for a whole screenful), or at $40a442
 * when the last row is done.
 *
 *  40a41e  move.w  D4, D1               ; row: left mask
 *  40a420  move.w  (-$4e,A6), D2        ;      words - 1
 *  40a424  jmp     (A1)
 *  40a426  adda.w  (-$4a,A6), A4        ; source modes end here
 *  40a42a  adda.w  A3, A5
 *  40a42c  dbra    D3, $40a41e
 *  40a430  bra     $40a442
 *  40a432  addq.w  #4, D7               ; pattern modes end here
 *  40a434  andi.w  #$3f, D7
 *  40a438  move.l  (-$40,A6,D7.w), D6   ; next pattern row
 *  40a43c  adda.w  A3, A5
 *  40a43e  dbra    D3, $40a41e
 *  40a442  ...                          ; epilogue
 *
 * srcCopy (shifted or inverted), and its last word:
 *  40a4c6  and.w   D5, D1
 *  40a4c8  move.l  (-$2,A4), D0
 *  40a4cc  adda.w  A0, A4
 *  40a4ce  lsr.l   D6, D0
 *  40a4d0  eor.w   D7, D0
 *  40a4d2  and.w   D1, D0
 *  40a4d4  not.w   D1
 *  40a4d6  and.w   (A5), D1
 *  40a4d8  or.w    D1, D0
 *  40a4da  move.w  D0, (A5)
 *  40a4dc  adda.w  A0, A5
 *  40a4de  moveq   #-$1, D1
 *  40a4e0  subq.w  #1, D2
 *  40a4e2  bgt     $40a4c8
 *  40a4e4  beq     $40a4c6
 *  40a4e6  bra     $40a426
 * srcOr/srcXor/srcBic ($40a546/$40a564/$40a582) are the same with
 * "or.w D0,(A5)", "eor.w D0,(A5)" or "not.w D0; and.w D0,(A5)" in place
 * of not/and/or/move.  Pattern modes ($40a5c6 copy, $40a664 or, $40a678
 * xor, $40a6fa bic, $40a5a2 one-word copy) use D6 for the source and
 * (A5)+.  Aligned srcCopy ($40a4a4 forward, $40a470 backward) and the
 * wide pattern copy/xor move the middle of each row with unrolled
 * move.l/eor.l runs entered through A2 (see blt_unrolled).
 */

#define BLT_CAP 2048    /* instructions per call, roughly (whole rows) */

typedef struct {
        uint32_t d0, d1, d2, d6, a2, a4, a5, sp, lo, n;
} blt_t;

static void wr16(uint32_t a, uint32_t v) { m68k_jit_write(a & 0xffffff, v & 0xffff, 2); }
static void wr32(uint32_t a, uint32_t v) { m68k_jit_write(a & 0xffffff, v, 4); }

#define SETW(r, v) ((r) = ((r) & 0xffff0000u) | ((v) & 0xffff))
#define SX(v) ((uint32_t)(int32_t)(int16_t)(v))

enum { U_COPY, U_FILL, U_XOR };

/* The unrolled middle-of-row movers, called by jsr (A2) with A2 either
 * the loop version at base or an entry into one of the two runs of 16
 * long moves that follow it (the second run ends with a word move):
 *
 *  base+$00  bclr    #$0, D2       ; (copy shown; fill is move.l D6,(A5)+
 *  base+$04  beq     base+$08      ;  and xor eor.l D6,(A5)+)
 *  base+$06  move.w  (A4)+, (A5)+
 *  base+$08  subi.w  #$20, D2
 *  base+$0c  ble     base+$12
 *  base+$0e  bsr     base+$18
 *  base+$10  bra     base+$08
 *  base+$12  neg.w   D2
 *  base+$14  jmp     ($2,PC,D2.w)  ; into the first run
 *  base+$18  move.l  (A4)+, (A5)+  ; x16
 *  base+$38  rts
 *  base+$3a  move.l  (A4)+, (A5)+  ; x15
 *  base+$58  move.w  (A4)+, (A5)+
 *  base+$5a  rts
 * Counts the rts back to the caller.
 */
static void blt_longs(blt_t *s, int kind, uint32_t k)
{
        for (uint32_t i = 0; i < k; i++) {
                uint32_t a5 = s->a5;
                if (kind == U_COPY) {
                        wr32(a5, rd32(s->a4));
                        s->a4 += 4;
                } else if (kind == U_FILL)
                        wr32(a5, s->d6);
                else
                        wr32(a5, rd32(a5) ^ s->d6);
                s->a5 = a5 + 4;
        }
        s->n += k;
}

static void blt_word(blt_t *s, int kind)
{
        if (kind == U_COPY) {
                wr16(s->a5, rd16(s->a4));
                s->a4 += 2;
        } else if (kind == U_FILL)
                wr16(s->a5, s->d6);
        else
                wr16(s->a5, rd16(s->a5) ^ s->d6);
        s->a5 += 2;
        s->n++;
}

static void blt_unrolled(blt_t *s, int kind, uint32_t base)
{
        uint32_t e = s->a2 & 0xffffff, run1 = base + 0x18, run2 = base + 0x3a;
        if (e == base) {
                s->n += 2;                              /* bclr, beq */
                int odd = s->d2 & 1;
                s->d2 &= ~1u;
                if (odd)
                        blt_word(s, kind);
                for (;;) {
                        int32_t r = (int16_t)s->d2 - 0x20;
                        SETW(s->d2, r);
                        s->n += 2;                      /* subi, ble */
                        if (r <= 0)
                                break;
                        wr32(s->sp - 8, s->lo | (base + 0x10));
                        s->n += 1;                      /* bsr */
                        blt_longs(s, kind, 16);
                        s->n += 2;                      /* rts, bra */
                }
                SETW(s->d2, -s->d2);
                s->n += 2;                              /* neg, jmp */
                e = run1 + (int16_t)s->d2;
        }
        if (e == run2 + 0x20) {
                /* just the rts */
        } else if (e < run2) {
                blt_longs(s, kind, (run1 + 0x20 - e) / 2);
        } else {
                blt_longs(s, kind, (run2 + 0x1e - e) / 2);
                blt_word(s, kind);
        }
        s->n++;                                         /* rts */
}

/* Is A2 a valid way into the unrolled mover at base, for rows of d2w words? */
static int blt_a2_ok(uint32_t a2, uint32_t base, int16_t d2w)
{
        a2 &= 0xffffff;
        if (a2 == base)
                return d2w > 0x20;
        if (a2 & 1)
                return 0;
        return a2 >= base + 0x18 && a2 <= base + 0x5a && a2 != base + 0x3a + 0x20;
}

static uint32_t bitblt_rows(jregs_t *j, uint32_t pc, uint32_t *ninstr)
{
        blt_t s;
        s.d0 = D(0), s.d1 = D(1), s.d2 = D(2), s.d6 = D(6);
        s.a2 = A(2), s.a4 = A(4), s.a5 = A(5), s.sp = A(7);
        s.lo = pc & 0xff000000u;
        s.n = 0;
        uint32_t d3 = D(3), d4 = D(4), d5 = D(5), d7 = D(7);
        uint32_t a0 = A(0), a1 = A(1), a3 = A(3), a6 = A(6);
        enum { T_SRC, T_PAT, T_PATW } tail = T_SRC;
        uint32_t d7old = 0, ret;

row:;
        uint32_t tgt = a1 & 0xffffff;
        int16_t d2w = (int16_t)rd16(a6 - 0x4e);
        int ok;
        switch (tgt) {
        case 0x40a4c8: case 0x40a4c6: case 0x40a548: case 0x40a546:
        case 0x40a566: case 0x40a564: case 0x40a584: case 0x40a582:
        case 0x40a666: case 0x40a664: case 0x40a6fc: case 0x40a6fa:
        case 0x40a5a2: case 0x40a694:
                ok = 1;
                break;
        case 0x40a4a4: case 0x40a5c6: case 0x40a678:
                ok = d2w >= 0;          /* (they pick A2 from it) */
                break;
        case 0x40a470:
                ok = d2w >= 1;          /* (0 would move 64K longs) */
                break;
        case 0x40a4b0:
                ok = blt_a2_ok(s.a2, 0x40a4ea, d2w);
                break;
        case 0x40a5e6:
                ok = blt_a2_ok(s.a2, 0x40a608, d2w);
                break;
        case 0x40a684:
                ok = blt_a2_ok(s.a2, 0x40a69e, d2w);
                break;
        default:
                ok = 0;
        }
        if (!ok) {
                if (s.n) {
                        ret = 0x40a41e;
                        goto out;
                }
                /* Something unexpected: just the row head, then the 68k */
                SETW(s.d1, d4);
                SETW(s.d2, d2w);
                D(1) = s.d1;
                D(2) = s.d2;
                j->n = (s.d2 >> 8) & 0xff;
                j->not_z = s.d2 & 0xffff;
                j->v = j->c = 0;
                *ninstr = 3;
                return a1;
        }
        SETW(s.d1, d4);
        SETW(s.d2, d2w);
        s.n += 3;                               /* move.w, move.w, jmp */
        int32_t o;

        switch (tgt) {
        case 0x40a4c6: goto L4c6;
        case 0x40a4c8: goto L4c8;
        case 0x40a546: goto L546;
        case 0x40a548: goto L548;
        case 0x40a564: goto L564;
        case 0x40a566: goto L566;
        case 0x40a582: goto L582;
        case 0x40a584: goto L584;
        case 0x40a4a4: goto L4a4;
        case 0x40a4b0: goto L4b0;
        case 0x40a470: goto L470;
        case 0x40a5c6: goto L5c6;
        case 0x40a5e6: goto L5e6;
        case 0x40a678: goto L678;
        case 0x40a684: goto L684;
        case 0x40a694: goto L694;
        case 0x40a664: goto L664;
        case 0x40a666: goto L666;
        case 0x40a6fa: goto L6fa;
        case 0x40a6fc: goto L6fc;
        case 0x40a5a2: goto L5a2;
        }

        /* ---- source modes: one word per pass, last word gets D5 too ---- */
#define SRC_FETCH() do {                                                \
                uint32_t sh = s.d6 & 63, v = rd32(s.a4 - 2);              \
                s.a4 += SX(a0);                                         \
                v = sh < 32 ? v >> sh : 0;                              \
                v ^= d7 & 0xffff;                                       \
                s.d0 = v & (s.d1 | 0xffff0000u);                        \
        } while (0)
#define SRC_NEXT(head, last) do {                                       \
                s.a5 += SX(a0);                                         \
                s.d1 = 0xffffffffu;                                     \
                o = (int16_t)s.d2;                                      \
                SETW(s.d2, o - 1);                                      \
                if (o > 1)                                              \
                        goto head;                                      \
                s.n++;                                  /* beq */       \
                if (o == 1)                                             \
                        goto last;                                      \
                s.n++;                                  /* bra */       \
                goto src_tail;                                          \
        } while (0)

L4c6:   SETW(s.d1, s.d1 & d5);
        s.n++;
L4c8:   SRC_FETCH();
        s.d1 ^= 0xffff;
        s.d1 &= rd16(s.a5) | 0xffff0000u;
        s.d0 |= s.d1 & 0xffff;
        wr16(s.a5, s.d0);
        s.n += 13;
        SRC_NEXT(L4c8, L4c6);

L546:   SETW(s.d1, s.d1 & d5);
        s.n++;
L548:   SRC_FETCH();
        wr16(s.a5, rd16(s.a5) | s.d0);
        s.n += 10;
        SRC_NEXT(L548, L546);

L564:   SETW(s.d1, s.d1 & d5);
        s.n++;
L566:   SRC_FETCH();
        wr16(s.a5, rd16(s.a5) ^ s.d0);
        s.n += 10;
        SRC_NEXT(L566, L564);

L582:   SETW(s.d1, s.d1 & d5);
        s.n++;
L584:   SRC_FETCH();
        s.d0 ^= 0xffff;
        wr16(s.a5, rd16(s.a5) & s.d0);
        s.n += 11;
        SRC_NEXT(L584, L582);

        /* ---- aligned srcCopy, forward ----
         *  40a4a4  lea     ($a,PC), A1     ; $40a4b0 from now on
         *  40a4a8  lea     ($40,PC), A2    ; $40a4ea
         *  40a4ac  bra     $40a5ce         ; pick the A2 entry for D2 words
         *  40a4b0  move.w  (A4)+, D0       ; first word under D1
         *  40a4b2  and.w   D1, D0
         *  40a4b4  not.w   D1
         *  40a4b6  and.w   (A5), D1
         *  40a4b8  or.w    D1, D0
         *  40a4ba  move.w  D0, (A5)+
         *  40a4bc  moveq   #-$1, D1
         *  40a4be  subq.w  #1, D2
         *  40a4c0  beq     $40a4c6         ; last word via the srcCopy loop
         *  40a4c2  jsr     (A2)
         *  40a4c4  moveq   #-$1, D2
         */
L4a4:   a1 = s.lo | 0x40a4b0;
        s.a2 = s.lo | 0x40a4ea;
        s.n += 3;
        goto L5ce_4b0;
L4b0:   SETW(s.d0, rd16(s.a4) & s.d1);
        s.a4 += 2;
        s.d1 ^= 0xffff;
        s.d1 &= rd16(s.a5) | 0xffff0000u;
        s.d0 |= s.d1 & 0xffff;
        wr16(s.a5, s.d0);
        s.a5 += 2;
        s.d1 = 0xffffffffu;
        o = (int16_t)s.d2;
        SETW(s.d2, o - 1);
        s.n += 9;
        if (o == 1)
                goto L4c6;
        wr32(s.sp - 4, s.lo | 0x40a4c4);
        s.n++;                                  /* jsr */
        blt_unrolled(&s, U_COPY, 0x40a4ea);
        s.d2 = 0xffffffffu;
        s.n++;                                  /* moveq */
        goto L4c6;

        /* ---- aligned srcCopy, backward ----
         *  40a470  move.w  (A4), D0            40a48e  move.w  -(A4), -(A5)
         *  40a472  adda.w  A0, A4              40a490  subq.w  #1, D2
         *  40a474  and.w   D1, D0              40a492  bra     $40a49a
         *  40a476  not.w   D1                  40a494  move.l  -(A4), -(A5)
         *  40a478  and.w   (A5), D1            40a496  move.l  -(A4), -(A5)
         *  40a47a  or.w    D1, D0              40a498  subq.w  #2, D2
         *  40a47c  move.w  D0, (A5)            40a49a  bgt     $40a494
         *  40a47e  adda.w  A0, A5              40a49c  beq     $40a496
         *  40a480  moveq   #-$1, D1            40a49e  adda.w  A0, A4
         *  40a482  subq.w  #1, D2              40a4a0  adda.w  A0, A5
         *  40a484  beq     $40a4c6             40a4a2  bra     $40a4c6
         *  40a486  suba.w  A0, A4
         *  40a488  suba.w  A0, A5
         *  40a48a  lsr.w   #1, D2
         *  40a48c  bcc     $40a496
         */
L470:   SETW(s.d0, rd16(s.a4) & s.d1);
        s.a4 += SX(a0);
        s.d1 ^= 0xffff;
        s.d1 &= rd16(s.a5) | 0xffff0000u;
        s.d0 |= s.d1 & 0xffff;
        wr16(s.a5, s.d0);
        s.a5 += SX(a0);
        s.d1 = 0xffffffffu;
        o = (int16_t)s.d2;
        SETW(s.d2, o - 1);
        s.n += 11;
        if (o == 1)
                goto L4c6;
        s.a4 -= SX(a0);
        s.a5 -= SX(a0);
        {
                uint32_t w = s.d2 & 0xffff;
                SETW(s.d2, w >> 1);
                s.n += 4;
                int32_t r;
                if (w & 1) {
                        s.a4 -= 2;
                        s.a5 -= 2;
                        wr16(s.a5, rd16(s.a4));
                        r = (int16_t)s.d2 - 1;
                        SETW(s.d2, r);
                        s.n += 3;
                        goto L49a;
                }
                goto L496;
L494:           s.a4 -= 4;
                s.a5 -= 4;
                wr32(s.a5, rd32(s.a4));
                s.n++;
L496:           s.a4 -= 4;
                s.a5 -= 4;
                wr32(s.a5, rd32(s.a4));
                r = (int16_t)s.d2 - 2;
                SETW(s.d2, r);
                s.n += 2;
L49a:           s.n++;                          /* bgt */
                if (r > 0)
                        goto L494;
                s.n++;                          /* beq */
                if (r == 0)
                        goto L496;
        }
        s.a4 += SX(a0);
        s.a5 += SX(a0);
        s.n += 3;
        goto L4c6;

        /* 40a5ce: pick the unrolled entry for D2 words (A2 = its base)
         *  40a5ce  cmpi.w  #$20, D2
         *  40a5d2  bgt     $40a5e4
         *  40a5d4  adda.w  #$39, A2
         *  40a5d8  btst    #$0, D2
         *  40a5dc  bne     $40a5e2
         *  40a5de  adda.w  #$21, A2
         *  40a5e2  suba.w  D2, A2
         *  40a5e4  jmp     (A1)
         */
#define PICK_A2() do {                                                  \
                int16_t w = (int16_t)s.d2;                              \
                s.n += 3;                                               \
                if (w <= 0x20) {                                        \
                        s.a2 += 0x39;                                   \
                        s.n += 4;                                       \
                        if (!(w & 1)) {                                 \
                                s.a2 += 0x21;                           \
                                s.n++;                                  \
                        }                                               \
                        s.a2 -= SX(w);                                  \
                }                                                       \
        } while (0)
L5ce_4b0:
        PICK_A2();
        goto L4b0;

        /* ---- pattern copy ----
         *  40a5c6  lea     ($1e,PC), A1    ; $40a5e6 from now on
         *  40a5ca  lea     ($3c,PC), A2    ; $40a608
         *  (40a5ce as above)
         *  40a5e6  move.w  D6, D0          40a5f8  move.w  D5, D1
         *  40a5e8  and.w   D1, D0          40a5fa  and.w   D1, D6
         *  40a5ea  not.w   D1              40a5fc  not.w   D1
         *  40a5ec  and.w   (A5), D1        40a5fe  and.w   (A5), D1
         *  40a5ee  or.w    D1, D0          40a600  or.w    D1, D6
         *  40a5f0  move.w  D0, (A5)+       40a602  move.w  D6, (A5)+
         *  40a5f2  subq.w  #1, D2          40a604  bra     $40a432
         *  40a5f4  beq     $40a5f8
         *  40a5f6  jsr     (A2)
         */
L5c6:   a1 = s.lo | 0x40a5e6;
        s.a2 = s.lo | 0x40a608;
        s.n += 2;
        PICK_A2();
L5e6:   SETW(s.d0, s.d6 & s.d1);
        s.d1 ^= 0xffff;
        s.d1 &= rd16(s.a5) | 0xffff0000u;
        s.d0 |= s.d1 & 0xffff;
        wr16(s.a5, s.d0);
        s.a5 += 2;
        o = (int16_t)s.d2;
        SETW(s.d2, o - 1);
        s.n += 8;
        if (o != 1) {
                wr32(s.sp - 4, s.lo | 0x40a5f8);
                s.n++;
                blt_unrolled(&s, U_FILL, 0x40a608);
        }
        SETW(s.d1, d5);
        s.d6 &= s.d1 | 0xffff0000u;
        s.d1 ^= 0xffff;
        s.d1 &= rd16(s.a5) | 0xffff0000u;
        s.d6 |= s.d1 & 0xffff;
        wr16(s.a5, s.d6);
        s.a5 += 2;
        s.n += 7;
        goto pat_tail;

        /* ---- pattern xor ----
         *  40a678  lea     ($a,PC), A1     ; $40a684 from now on
         *  40a67c  lea     ($20,PC), A2    ; $40a69e
         *  40a680  bra     $40a5ce
         *  40a684  move.w  D6, D0          40a694  and.w   D5, D1
         *  40a686  and.w   D1, D0          40a696  and.w   D1, D6
         *  40a688  eor.w   D0, (A5)+       40a698  eor.w   D6, (A5)+
         *  40a68a  moveq   #-$1, D1        40a69a  bra     $40a432
         *  40a68c  subq.w  #1, D2
         *  40a68e  beq     $40a694
         *  40a690  jsr     (A2)
         *  40a692  moveq   #-$1, D2
         */
L678:   a1 = s.lo | 0x40a684;
        s.a2 = s.lo | 0x40a69e;
        s.n += 3;
        PICK_A2();
L684:   SETW(s.d0, s.d6 & s.d1);
        wr16(s.a5, rd16(s.a5) ^ s.d0);
        s.a5 += 2;
        s.d1 = 0xffffffffu;
        o = (int16_t)s.d2;
        SETW(s.d2, o - 1);
        s.n += 6;
        if (o != 1) {
                wr32(s.sp - 4, s.lo | 0x40a692);
                s.n++;
                blt_unrolled(&s, U_XOR, 0x40a69e);
                s.d2 = 0xffffffffu;
                s.n++;
        }
L694:   SETW(s.d1, s.d1 & d5);
        s.d6 &= s.d1 | 0xffff0000u;
        wr16(s.a5, rd16(s.a5) ^ s.d6);
        s.a5 += 2;
        s.n += 4;
        goto pat_tail;

        /* ---- pattern or / bic ----
         *  40a664  and.w   D5, D1          40a6fa  and.w   D5, D1
         *  40a666  move.w  D6, D0          40a6fc  move.w  D6, D0
         *  40a668  and.w   D1, D0          40a6fe  and.w   D1, D0
         *  40a66a  or.w    D0, (A5)+       40a700  not.w   D0
         *  40a66c  moveq   #-$1, D1        40a702  and.w   D0, (A5)+
         *  40a66e  subq.w  #1, D2          (moveq, subq, bgt, beq, bra
         *  40a670  bgt     $40a666          as for or)
         *  40a672  beq     $40a664
         *  40a674  bra     $40a432
         */
#define PAT_NEXT(head, last) do {                                       \
                s.a5 += 2;                                              \
                s.d1 = 0xffffffffu;                                     \
                o = (int16_t)s.d2;                                      \
                SETW(s.d2, o - 1);                                      \
                if (o > 1)                                              \
                        goto head;                                      \
                s.n++;                                                  \
                if (o == 1)                                             \
                        goto last;                                      \
                s.n++;                                                  \
                goto pat_tail;                                          \
        } while (0)
L664:   SETW(s.d1, s.d1 & d5);
        s.n++;
L666:   SETW(s.d0, s.d6 & s.d1);
        wr16(s.a5, rd16(s.a5) | s.d0);
        s.n += 6;
        PAT_NEXT(L666, L664);

L6fa:   SETW(s.d1, s.d1 & d5);
        s.n++;
L6fc:   SETW(s.d0, ~(s.d6 & s.d1));
        wr16(s.a5, rd16(s.a5) & s.d0);
        s.n += 7;
        PAT_NEXT(L6fc, L6fa);

        /* ---- one-word pattern copy: does all the rows itself ----
         *  40a5a2  and.w   D5, D1          40a5b2  adda.w  A3, A5
         *  40a5a4  move.w  D1, D5          40a5b4  addq.w  #4, D7
         *  40a5a6  not.w   D5              40a5b6  andi.w  #$3f, D7
         *  40a5a8  and.w   D1, D6          40a5ba  move.w  (-$40,A6,D7.w), D6
         *  40a5aa  move.w  (A5), D0        40a5be  dbra    D3, $40a5a8
         *  40a5ac  and.w   D5, D0          40a5c2  bra     $40a442
         *  40a5ae  or.w    D6, D0
         *  40a5b0  move.w  D0, (A5)+
         */
L5a2:   SETW(s.d1, s.d1 & d5);
        SETW(d5, ~s.d1);
        s.n += 3;
        for (;;) {
                s.d6 &= s.d1 | 0xffff0000u;
                SETW(s.d0, (rd16(s.a5) & d5) | s.d6);
                wr16(s.a5, s.d0);
                s.a5 += 2 + SX(a3);
                d7old = d7;
                SETW(d7, (d7 + 4) & 0x3f);
                SETW(s.d6, rd16(a6 - 0x40 + SX(d7)));
                s.n += 10;
                SETW(d3, d3 - 1);
                if ((d3 & 0xffff) == 0xffff)
                        break;
        }
        s.n++;
        tail = T_PATW;
        ret = 0x40a442;
        goto out;

src_tail:
        s.a4 += SX(rd16(a6 - 0x4a));
        s.a5 += SX(a3);
        s.n += 3;
        tail = T_SRC;
        SETW(d3, d3 - 1);
        if ((d3 & 0xffff) == 0xffff) {
                s.n++;                          /* bra $40a442 */
                ret = 0x40a442;
                goto out;
        }
        goto next_row;

pat_tail:
        d7old = d7;
        SETW(d7, (d7 + 4) & 0x3f);
        s.d6 = rd32(a6 - 0x40 + SX(d7));
        s.a5 += SX(a3);
        s.n += 5;
        tail = T_PAT;
        SETW(d3, d3 - 1);
        if ((d3 & 0xffff) == 0xffff) {
                ret = 0x40a442;
                goto out;
        }
next_row:
        if (s.n < BLT_CAP)
                goto row;
        ret = 0x40a41e;

out:
        D(0) = s.d0, D(1) = s.d1, D(2) = s.d2, D(3) = d3;
        D(5) = d5, D(6) = s.d6, D(7) = d7;
        A(1) = a1, A(2) = s.a2, A(4) = s.a4, A(5) = s.a5;
        if (tail == T_SRC) {
                /* subq.w #1, D2 */
                uint32_t res = s.d2 & 0xffff, old = (res + 1) & 0xffff;
                uint32_t r = old - 1;
                j->x = j->c = (r >> 8) & 0x100;
                j->n = (res >> 8) & 0xff;
                j->not_z = res;
                j->v = ((1 ^ old) & (r ^ old)) >> 8 & 0x80;
        } else {
                /* addq.w #4, D7 (X); then move.l / move.w into D6 */
                j->x = (((d7old & 0xffff) + 4) >> 8) & 0x100;
                if (tail == T_PAT) {
                        j->n = s.d6 >> 24;
                        j->not_z = s.d6;
                } else {
                        j->n = (s.d6 >> 8) & 0xff;
                        j->not_z = s.d6 & 0xffff;
                }
                j->v = j->c = 0;
        }
        *ninstr = s.n;
        return s.lo | ret;
}

const m68k_native_t m68k_natives[] = {
        { 0x413dee, get_resource, "GetResource" },
        { 0x401e0a, set_cursor, "SetCursor" },
        { 0x40a110, sect_rects, "rect intersection" },
        { 0x4138ca, ref_max_below, "ref scan (max below)" },
        { 0x41405e, ref_find_handle, "ref scan (handle)" },
        { 0x4106c2, heap_find_free, "heap free-block search" },
        { 0x411d72, front_window, "FrontWindow" },
        { 0x413f10, find_ref_id, "find resource ID" },
        { 0x413f1e, find_type, "find resource type" },
        { 0x415dac, unit_scan, "SystemTask driver scan" },
        { 0x40a184, union_rect, "UnionRect" },
        { 0x40a41e, bitblt_rows, "bitblt rows" },
};
const int m68k_native_count = sizeof m68k_natives / sizeof m68k_natives[0];
struct m68k_native_stat m68k_native_stats[sizeof m68k_natives / sizeof m68k_natives[0]];

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
