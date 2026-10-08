/* Exhaustive check of t_modimm against the architectural ThumbExpandImm */
#include <stdio.h>
#include "../../src/jit/thumb.h"

static uint32_t expand(int imm12)
{
        uint32_t b = imm12 & 0xff;
        if ((imm12 >> 10) == 0) {
                switch (imm12 >> 8 & 3) {
                case 0: return b;
                case 1: return b | b << 16;
                case 2: return b << 8 | b << 24;
                default: return b * 0x01010101u;
                }
        }
        uint32_t x = 0x80 | (imm12 & 0x7f);
        int rot = imm12 >> 7;
        return (x >> rot) | (x << (32 - rot));
}

int main(void)
{
        int bad = 0;
        for (int enc = 0; enc < 4096; enc++) {
                if ((enc >> 10) == 0 && (enc >> 8 & 3) && (enc & 0xff) == 0)
                        continue;       /* UNPREDICTABLE */
                uint32_t v = expand(enc);
                int m = t_modimm(v);
                if (m < 0 || expand(m) != v) {
                        if (bad++ < 10)
                                printf("value %08x (enc %03x): got %d\n", v, enc, m);
                }
        }
        /* and some values that must NOT be encodable */
        uint32_t no[] = { 0x101, 0x12345678, 0x1fe00001, 0xab00ab01 };
        for (unsigned i = 0; i < sizeof no / sizeof *no; i++) {
                int m = t_modimm(no[i]);
                if (m >= 0 && expand(m) != no[i]) { printf("bogus encoding for %08x\n", no[i]); bad++; }
        }
        printf("%s (%d bad)\n", bad ? "FAIL" : "all encodable values round-trip", bad);
        return bad != 0;
}
