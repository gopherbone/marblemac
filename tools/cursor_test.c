/* Host test: render the standard Mac arrow at several rotations as ASCII. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "../src/cursor.h"

static const mac_cursor_t arrow = {
        { 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
          0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 },
        { 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
          0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380 },
        1, 1
};

int main(void)
{
        float dirs[] = { -135, -90, 0, 45, 90, 180 };
        for (unsigned k = 0; k < sizeof dirs / sizeof *dirs; k++) {
                enum { W = 48, H = 48, S = W / 8 };
                uint8_t fw[S * H], fb[S * H];
                memset(fw, 0xff, sizeof fw);
                memset(fb, 0x00, sizeof fb);
                float dir = dirs[k] * (float)M_PI / 180;
                cursor_draw(fw, S, W, H, &arrow, 24, 24, dir - CURSOR_ARROW_ANGLE);
                cursor_draw(fb, S, W, H, &arrow, 24, 24, dir - CURSOR_ARROW_ANGLE);
                printf("moving %.0f deg (0=right, 90=down)\n", dirs[k]);
                for (int y = 8; y < H - 8; y++) {
                        for (int x = 4; x < W - 4; x++) {
                                int a = (fw[y*S + x/8] >> (7 - (x&7))) & 1;
                                int b = (fb[y*S + x/8] >> (7 - (x&7))) & 1;
                                char ch = (a && b) ? 'o' : (!a && !b) ? '#' : '.';
                                putchar(x == 24 && y == 24 ? '+' : ch);
                        }
                        putchar('\n');
                }
        }
        return 0;
}
