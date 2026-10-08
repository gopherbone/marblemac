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
        { 0x413f10, find_ref_id, "find resource ID" },
        { 0x413f1e, find_type, "find resource type" },
        { 0x415dac, unit_scan, "SystemTask driver scan" },
        { 0x40a184, union_rect, "UnionRect" },
        { 0x40a41e, bitblt_rows, "bitblt rows" },
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
