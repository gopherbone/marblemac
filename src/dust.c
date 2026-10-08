/* Dust: a small pool of specks, each a pixel or a 2x2 block, that puff
 * out from the wall, slow down quickly, drift downhill with the tilt and
 * flicker out.
 *
 * On a 1-bit screen a black speck vanishes on black and a white one on
 * white, and either kind barely shows on the Mac's 50% grey checkerboard,
 * so each speck looks at the pixels around it when drawn and picks its
 * ink: black on light areas, white on dark ones, and a solid 2x2 black
 * blob on grey.
 */

#include <math.h>
#include "dust.h"

#define NDUST           96
#define DRAG            5.0f    /* 1/s: puffs stop fast */
#define FALL            0.2f    /* fraction of the marble's tilt accel */

typedef struct {
        float x, y, vx, vy;
        float t, life;
        uint8_t big, alive;
} speck_t;

static speck_t specks[NDUST];
static int next_speck;
static unsigned int frame_no;

void dust_spawn(float x, float y, float vx, float vy, float life, int big)
{
        speck_t *s = &specks[next_speck];
        next_speck = (next_speck + 1) % NDUST;  /* oldest goes first */
        s->x = x;
        s->y = y;
        s->vx = vx;
        s->vy = vy;
        s->t = 0;
        s->life = life;
        s->big = big != 0;
        s->alive = 1;
}

void dust_step(float dt, float gx, float gy, float w, float h)
{
        float drag = expf(-DRAG * dt);
        for (int i = 0; i < NDUST; i++) {
                speck_t *s = &specks[i];
                if (!s->alive)
                        continue;
                s->t += dt;
                if (s->t >= s->life) {
                        s->alive = 0;
                        continue;
                }
                s->vx = (s->vx + gx * FALL * dt) * drag;
                s->vy = (s->vy + gy * FALL * dt) * drag;
                s->x += s->vx * dt;
                s->y += s->vy * dt;
                /* Settle against the edges rather than leaving the screen */
                if (s->x < 0) {
                        s->x = 0;
                        s->vx = 0;
                } else if (s->x > w - 1) {
                        s->x = w - 1;
                        s->vx = 0;
                }
                if (s->y < 0) {
                        s->y = 0;
                        s->vy = 0;
                } else if (s->y > h - 1) {
                        s->y = h - 1;
                        s->vy = 0;
                }
        }
}

static inline uint32_t hash(uint32_t x)
{
        x ^= x >> 16;
        x *= 0x7feb352du;
        x ^= x >> 15;
        x *= 0x846ca68bu;
        x ^= x >> 16;
        return x;
}

static inline void plot(uint8_t *frame, int rowbytes, int width, int height, int x, int y, int white)
{
        if ((unsigned)x >= (unsigned)width || (unsigned)y >= (unsigned)height)
                return;
        uint8_t *p = frame + y * rowbytes + (x >> 3);
        uint8_t m = 0x80 >> (x & 7);
        if (white)
                *p |= m;
        else
                *p &= ~m;
}

static inline int bits8(unsigned int b)
{
        b = b - ((b >> 1) & 0x55);
        b = (b & 0x33) + ((b >> 2) & 0x33);
        return (b + (b >> 4)) & 0x0f;
}

/* Fraction of white pixels in the 8x3 block around (x, y) */
static float whiteness(const uint8_t *frame, int rowbytes, int width, int height, int x, int y)
{
        int set = 0, n = 0;
        int bx = (x < 0 ? 0 : x >= width ? width - 1 : x) >> 3;
        for (int yy = y - 1; yy <= y + 1; yy++) {
                if (yy < 0 || yy >= height)
                        continue;
                set += bits8(frame[yy * rowbytes + bx]);
                n += 8;
        }
        return n ? (float)set / n : 1.0f;
}

void dust_draw(uint8_t *frame, int rowbytes, int width, int height, int cam_x, int cam_y)
{
        frame_no++;
        for (int i = 0; i < NDUST; i++) {
                speck_t *s = &specks[i];
                if (!s->alive)
                        continue;
                float age = s->t / s->life;
                /* Second half of its life: flicker out, showing on fewer
                 * and fewer frames
                 */
                if (age > 0.5f) {
                        float keep = (1.0f - age) * 2.0f;
                        if ((hash(frame_no * 131u + i) & 0xff) >= keep * 256.0f)
                                continue;
                }
                int x = (int)floorf(s->x) - cam_x;
                int y = (int)floorf(s->y) - cam_y;
                if (x < -1 || y < -1 || x >= width || y >= height)
                        continue;

                int size = (s->big && age < 0.45f) ? 2 : 1;
                float wf = whiteness(frame, rowbytes, width, height, x, y);
                int white;
                if (wf > 0.65f) {
                        white = 0;
                } else if (wf < 0.35f) {
                        white = 1;
                } else {
                        /* grey: only a solid blob shows */
                        white = 0;
                        size = 2;
                }
                plot(frame, rowbytes, width, height, x, y, white);
                if (size == 2) {
                        plot(frame, rowbytes, width, height, x + 1, y, white);
                        plot(frame, rowbytes, width, height, x, y + 1, white);
                        plot(frame, rowbytes, width, height, x + 1, y + 1, white);
                }
        }
}
