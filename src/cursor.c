/* Rotated Mac cursor rendering.
 *
 * Draws a QuickDraw cursor into a Playdate-style 1bpp framebuffer,
 * optionally rotated around its hotspot.  Rotated cursors are
 * supersampled (3x3) so the thin white outline of the arrow survives
 * at awkward angles.
 */

#include <math.h>
#include "cursor.h"

/* The arrow's body lies between straight down and down-right (its left
 * edge is vertical, its other edge is at 45 degrees), so its tip points
 * 22.5 degrees left of straight up.
 */
const float CURSOR_ARROW_ANGLE = -1.9634954f;   /* atan2(-cos 22.5, -sin 22.5) */

#define ROT_RADIUS      23      /* > distance from hotspot to far corner */
#define SS              3       /* supersampling per axis */

static inline void      px_black(uint8_t *f, int stride, int x, int y)
{
        f[y * stride + (x >> 3)] &= ~(0x80 >> (x & 7));
}

static inline void      px_white(uint8_t *f, int stride, int x, int y)
{
        f[y * stride + (x >> 3)] |= (0x80 >> (x & 7));
}

static inline void      px_invert(uint8_t *f, int stride, int x, int y)
{
        f[y * stride + (x >> 3)] ^= (0x80 >> (x & 7));
}

static inline int       bit(const uint16_t *rows, int u, int v)
{
        if ((unsigned)u > 15 || (unsigned)v > 15)
                return 0;
        return (rows[v] >> (15 - u)) & 1;
}

int     cursor_is_pointer(const mac_cursor_t *c)
{
        return c->hot_h <= 2 && c->hot_v <= 2;
}

void    cursor_extent(const mac_cursor_t *c, float rotation,
                      float *minx, float *maxx, float *miny, float *maxy)
{
        float cs = cosf(rotation), sn = sinf(rotation);
        float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;

        for (int v = 0; v < 16; v++) {
                for (int u = 0; u < 16; u++) {
                        if (!bit(c->mask, u, v))
                                continue;
                        /* Pixel corners relative to the hotspot pixel's centre */
                        for (int k = 0; k < 4; k++) {
                                float px = u - c->hot_h - 0.5f + (k & 1);
                                float py = v - c->hot_v - 0.5f + (k >> 1);
                                float rx = cs * px - sn * py;
                                float ry = sn * px + cs * py;
                                x0 = fminf(x0, rx); x1 = fmaxf(x1, rx);
                                y0 = fminf(y0, ry); y1 = fmaxf(y1, ry);
                        }
                }
        }
        if (x0 > x1)
                x0 = x1 = y0 = y1 = 0;  /* blank cursor */
        *minx = x0; *maxx = x1; *miny = y0; *maxy = y1;
}

static void     draw_upright(uint8_t *frame, int stride, int width, int height,
                             const mac_cursor_t *c, int x, int y)
{
        int ox = x - c->hot_h;
        int oy = y - c->hot_v;

        for (int v = 0; v < 16; v++) {
                int sy = oy + v;
                if (sy < 0 || sy >= height)
                        continue;
                for (int u = 0; u < 16; u++) {
                        int sx = ox + u;
                        if (sx < 0 || sx >= width)
                                continue;
                        int d = bit(c->data, u, v);
                        int m = bit(c->mask, u, v);
                        /* QuickDraw: mask+data = black, mask only = white,
                         * data only = invert, neither = transparent.
                         */
                        if (m)
                                d ? px_black(frame, stride, sx, sy) : px_white(frame, stride, sx, sy);
                        else if (d)
                                px_invert(frame, stride, sx, sy);
                }
        }
}

void    cursor_draw(uint8_t *frame, int stride, int width, int height,
                    const mac_cursor_t *c, int x, int y, float rotation)
{
        if (fabsf(rotation) < 0.02f) {
                draw_upright(frame, stride, width, height, c, x, y);
                return;
        }

        float cs = cosf(rotation);
        float sn = sinf(rotation);
        /* Hotspot point is the centre of the hotspot pixel */
        float hx = c->hot_h + 0.5f;
        float hy = c->hot_v + 0.5f;

        for (int dy = -ROT_RADIUS; dy <= ROT_RADIUS; dy++) {
                int sy = y + dy;
                if (sy < 0 || sy >= height)
                        continue;
                for (int dx = -ROT_RADIUS; dx <= ROT_RADIUS; dx++) {
                        int sx = x + dx;
                        if (sx < 0 || sx >= width)
                                continue;

                        int m = 0, d = 0;
                        for (int j = 0; j < SS; j++) {
                                float py = dy + (j - (SS - 1) / 2.0f) / SS;
                                for (int i = 0; i < SS; i++) {
                                        float px = dx + (i - (SS - 1) / 2.0f) / SS;
                                        /* Inverse-rotate screen offset back into cursor space */
                                        float u = cs * px + sn * py + hx;
                                        float v = -sn * px + cs * py + hy;
                                        int iu = (int)floorf(u);
                                        int iv = (int)floorf(v);
                                        if (bit(c->mask, iu, iv)) {
                                                m++;
                                                d += bit(c->data, iu, iv);
                                        }
                                }
                        }

                        /* Mostly-covered pixels are opaque; colour by
                         * majority, biased slightly towards white so the
                         * outline stays visible.
                         */
                        if (m * 2 >= SS * SS - 1) {
                                if (d * 2 > m + 1)
                                        px_black(frame, stride, sx, sy);
                                else
                                        px_white(frame, stride, sx, sy);
                        }
                }
        }
}
