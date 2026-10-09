/* Lander mode: the cursor as a little rocket.
 *
 * Gravity is constant and straight down the screen; the engine pushes
 * along the nose.  Velocity is integrated semi-implicitly (accelerate,
 * then move), drag is exact (exp), and every speed is capped, so it's
 * stable at any frame time the game hands us (up to 0.1s).  Walls are
 * the marble's: the whole drawn cursor stays on the Mac screen, and
 * hitting an edge keeps only wall_bounce of the speed into it.  Sitting
 * on the floor, a bit of friction stops it skating about forever.
 *
 * The exhaust is drawn on the Playdate frame like the dust and the rope:
 * a flickering teardrop out of the back of the arrow, black with a white
 * halo so it shows on anything.  Fired close to a wall, the jet blows
 * dust off it.
 */

#include <math.h>
#include "lander.h"
#include "dust.h"

#define CRANK_FOLLOW    25.0f   /* 1/s: how fast the nose catches the crank */
#define FLOOR_DIST      0.75f   /* px: resting on the floor */

#define FLAME_BACK      13.0f   /* px from the tip to where the flame starts */
#define FLAME_LEN       13.0f   /* px, full length (it flickers 60-110% of this) */
#define FLAME_WIDTH     2.6f    /* px, radius at the root */
#define FLAME_ON        0.06f   /* s to light up */
#define FLAME_OFF       0.10f   /* s to die down */
#define JET_REACH       48.0f   /* px past the flame's root that blows dust */
#define JET_DUST        70.0f   /* specks/s with the nozzle right on a wall */

float lander_crank_heading(float deg)
{
        return remainderf(deg * ((float)M_PI / 180) - (float)M_PI / 2, 2 * (float)M_PI);
}

float lander_steer(float heading, const lander_steer_t *in, const lander_params_t *p, float dt)
{
        if (!in->steer || !(dt > 0))
                return heading;
        if (in->docked) {
                heading += in->rot * p->turn * ((float)M_PI / 180) * dt;
        } else {
                float diff = remainderf(lander_crank_heading(in->crank_deg) - heading, 2 * (float)M_PI);
                heading += diff * (1 - expf(-CRANK_FOLLOW * dt));
        }
        return remainderf(heading, 2 * (float)M_PI);
}

void lander_accel(float heading, int thrust, const lander_params_t *p, float *ax, float *ay)
{
        *ax = 0;
        *ay = p->gravity;
        if (thrust) {
                *ax += cosf(heading) * p->thrust;
                *ay += sinf(heading) * p->thrust;
        }
}

void lander_move(float *x, float *y, float *vx, float *vy, const lander_params_t *p,
                 float x0, float x1, float y0, float y1, float w, float h,
                 float dt, float *in_vx, float *in_vy)
{
        float lo_x = -x0, hi_x = w - x1;
        float lo_y = -y0, hi_y = h - y1;

        float drag = expf(-p->drag * dt);
        *vx *= drag;
        *vy *= drag;

        /* Landed: the feet grip a little */
        if (*y >= hi_y - FLOOR_DIST && *vy >= 0) {
                float dv = p->ground_friction * dt;
                if (fabsf(*vx) <= dv)
                        *vx = 0;
                else
                        *vx -= copysignf(dv, *vx);
        }

        float sp = sqrtf(*vx * *vx + *vy * *vy);
        if (sp > p->max_speed) {
                *vx *= p->max_speed / sp;
                *vy *= p->max_speed / sp;
        }

        *x += *vx * dt;
        *y += *vy * dt;

        *in_vx = *vx;
        *in_vy = *vy;
        if (*x < lo_x) {
                *x = lo_x;
                if (*vx < 0)
                        *vx = -*vx * p->wall_bounce;
        } else if (*x > hi_x) {
                *x = hi_x;
                if (*vx > 0)
                        *vx = -*vx * p->wall_bounce;
        }
        if (*y < lo_y) {
                *y = lo_y;
                if (*vy < 0)
                        *vy = -*vy * p->wall_bounce;
        } else if (*y > hi_y) {
                *y = hi_y;
                if (*vy > 0)
                        *vy = -*vy * p->wall_bounce;
        }
}

////////////////////////////////////////////////////////////////////////////////
// Exhaust

static float flame;             /* 0 (out) .. 1 (full) */
static float flame_len;         /* this frame's flicker */
static float flame_wob;         /* sideways wobble, -1..1 */
static float jet_acc;
static uint32_t rng = 0x6A09E667u;

static float frand(void)
{
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (rng >> 8) * (1.0f / 16777216.0f);
}

void lander_flame_step(int thrust, float x, float y, float heading, float w, float h,
                       float dust, float dt)
{
        if (!(dt > 0))
                return;
        float k = thrust ? dt / FLAME_ON : -dt / FLAME_OFF;
        flame = fmaxf(0, fminf(1, flame + k));
        flame_len = 0.6f + 0.5f * frand();
        flame_wob = frand() * 2 - 1;
        if (flame <= 0 || !thrust) {
                jet_acc = 0;
                return;
        }

        /* Where does the jet meet a wall?  Ray from the flame's root
         * along the exhaust direction.
         */
        float dx = -cosf(heading), dy = -sinf(heading);
        float bx = x + dx * FLAME_BACK, by = y + dy * FLAME_BACK;
        float t = JET_REACH;
        int wall = -1;                  /* 0/1: left/right, 2/3: top/bottom */
        if (dx < -1e-3f && -bx / dx < t) {
                t = -bx / dx;
                wall = 0;
        } else if (dx > 1e-3f && (w - 1 - bx) / dx < t) {
                t = (w - 1 - bx) / dx;
                wall = 1;
        }
        if (dy < -1e-3f && -by / dy < t) {
                t = -by / dy;
                wall = 2;
        } else if (dy > 1e-3f && (h - 1 - by) / dy < t) {
                t = (h - 1 - by) / dy;
                wall = 3;
        }
        if (wall < 0) {
                jet_acc = 0;
                return;
        }
        t = fmaxf(0, t);
        float close = 1 - t / JET_REACH;
        float hx = fmaxf(0.5f, fminf(w - 1.5f, bx + dx * t));
        float hy = fmaxf(0.5f, fminf(h - 1.5f, by + dy * t));

        jet_acc += dt * JET_DUST * close * flame * dust;
        while (jet_acc >= 1) {
                jet_acc -= 1;
                /* Blown out sideways along the wall, a little off it */
                float side = frand() < 0.5f ? -1.0f : 1.0f;
                float along = side * (40 + 200 * close) * (0.4f + 0.6f * frand());
                float off = (10 + 60 * close) * frand();
                float life = 0.2f + 0.35f * frand();
                int big = frand() < 0.3f * close;
                if (wall < 2) {
                        float nx = wall == 0 ? 1.0f : -1.0f;
                        dust_spawn(hx, hy, nx * off, along + dy * 60, life, big);
                } else {
                        float ny = wall == 2 ? 1.0f : -1.0f;
                        dust_spawn(hx, hy, along + dx * 60, ny * off, life, big);
                }
        }
}

static uint8_t *fb;
static int fb_rb, fb_w, fb_h;

static inline void plot(int x, int y, int white)
{
        if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h)
                return;
        uint8_t *p = fb + y * fb_rb + (x >> 3);
        uint8_t m = 0x80 >> (x & 7);
        if (white)
                *p |= m;
        else
                *p &= ~m;
}

static void disc(float cx, float cy, float r, int white)
{
        int x0 = (int)floorf(cx - r), x1 = (int)ceilf(cx + r);
        int y0 = (int)floorf(cy - r), y1 = (int)ceilf(cy + r);
        float r2 = r * r;
        for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                        float ex = x + 0.5f - cx, ey = y + 0.5f - cy;
                        if (ex * ex + ey * ey <= r2)
                                plot(x, y, white);
                }
}

void lander_flame_draw(uint8_t *frame, int rowbytes, int width, int height,
                       int x, int y, float heading)
{
        if (flame <= 0)
                return;
        fb = frame;
        fb_rb = rowbytes;
        fb_w = width;
        fb_h = height;

        float dx = -cosf(heading), dy = -sinf(heading);        /* out the back */
        float px = -dy, py = dx;                                /* sideways */
        float len = FLAME_LEN * flame * flame_len;
        float rad = FLAME_WIDTH * (0.5f + 0.5f * flame);
        float cx0 = x + 0.5f + dx * FLAME_BACK, cy0 = y + 0.5f + dy * FLAME_BACK;
        int n = (int)len + 1;

        /* pass 0: white halo, pass 1: black core */
        for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i <= n; i++) {
                        float s = len * i / n, u = s / fmaxf(len, 1);
                        /* fat at the root, a wobbly point at the end */
                        float r = rad * sqrtf(fmaxf(0, 1 - u)) + 0.4f;
                        float wob = flame_wob * u * u * 2.0f;
                        float cx = cx0 + dx * s + px * wob, cy = cy0 + dy * s + py * wob;
                        disc(cx, cy, pass ? r : r + 1.2f, !pass);
                }
                /* A spark or two off the end */
                if (flame > 0.5f) {
                        uint32_t save = rng;
                        for (int k = 0; k < 2; k++) {
                                float s = len + 2 + 5 * frand(), side = (frand() * 2 - 1) * 2.5f;
                                float sx = cx0 + dx * s + px * side, sy = cy0 + dy * s + py * side;
                                if (frand() < 0.6f)
                                        disc(sx, sy, pass ? 0.75f : 1.8f, !pass);
                        }
                        if (!pass)
                                rng = save;     /* same sparks in both passes */
                }
        }
}
