/* Wall contact -> thunks, scraping and dust.
 *
 * Each of the four screen edges keeps a little state:
 *
 *  - Impacts: the walls clamp the cursor exactly onto them, so "clamped
 *    this step while moving into the wall" is a hit, and the speed into
 *    the wall before the bounce is how hard.  A wall only thunks if it's
 *    "armed": the marble must have been clearly away from it (REARM_DIST)
 *    since its last hit.  Leaning on a wall makes the marble micro-bounce
 *    against it every frame (tilt pushes it in, the bounce nudges it a
 *    fraction of a pixel back out), and those never re-arm it, so resting
 *    contact is silent while real bounces (which leave the wall) each get
 *    their own, smaller, thunk.  Plus a short cooldown per wall.
 *
 *  - Sliding: within TOUCH_DIST of a wall (held for a moment to bridge
 *    the micro-bounces) counts as touching, and the speed along the wall
 *    drives the scrape's level and a trickle of dust.
 *
 *  - The lander is slower than the marble, so its hits are judged on a
 *    gentler scale, and coming down on the floor fast is a hard landing:
 *    a heavier crash and a much bigger dust cloud, thrown up and out.
 */

#include <math.h>
#include <stdint.h>
#include "wallfx.h"
#include "sfx.h"
#include "dust.h"
#include "tuning.h"

#define HIT_EPS         0.01f   /* px: clamped onto the wall */
#define TOUCH_DIST      1.5f    /* px: close enough to scrape */
#define REARM_DIST      3.0f    /* px: left the wall, can thunk again */
#define TOUCH_HOLD      0.10f   /* s */
#define COOLDOWN        0.06f   /* s between thunks off one wall */

#define THUNK_MIN       70.0f   /* px/s into the wall: quieter is silent */
#define THUNK_FULL      1200.0f /* px/s for the hardest thunk */
#define SCRAPE_MIN      20.0f   /* px/s along the wall */
#define SCRAPE_FULL     700.0f

#define PUFF_BASE       3.0f    /* specks for the softest thunk */
#define PUFF_HARD       10.0f   /* extra specks for the hardest */
#define SCRAPE_RATE     28.0f   /* specks/s scraping at full speed */

#define LANDER_THUNK_FULL 600.0f /* px/s for the hardest lander thunk */
#define HARD_LANDING    200.0f  /* px/s down into the floor: a crash */
#define CRASH_FULL      480.0f  /* px/s for the worst crash */
#define CRASH_SPECKS    34.0f   /* extra specks for the worst crash */

enum { LEFT, RIGHT, TOP, BOTTOM };

typedef struct {
        int armed;
        float hold;             /* touching for this much longer */
        float cooldown;
        float dust_acc;
} wall_t;

static wall_t walls[4] = { { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 }, { 1, 0, 0, 0 } };
static uint32_t rng = 0x1234567u;

static float frand(void)
{
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        return (rng >> 8) * (1.0f / 16777216.0f);
}

static float clamp01(float v)
{
        return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Spawn a speck at `along` px along wall i from point (px, py), moving
 * `out` px/s away from the wall and `tan` px/s along it.
 */
static void speck(int i, float px, float py, float along, float out, float tan, float life, int big)
{
        if (i == LEFT || i == RIGHT) {
                float nx = (i == LEFT) ? 1.0f : -1.0f;
                dust_spawn(px, py + along, nx * out, tan, life, big);
        } else {
                float ny = (i == TOP) ? 1.0f : -1.0f;
                dust_spawn(px + along, py, tan, ny * out, life, big);
        }
}

/* A hard landing at (px, py) on the floor: a wide, tall cloud, flung up
 * and out to both sides, c 0..1 how hard */
static void crash_dust(float px, float py, float span, float c, float vt, float dust_amt)
{
        int n = (int)(CRASH_SPECKS * (0.4f + 0.6f * c) * dust_amt + 0.5f);
        float sp = fminf(span, 16.0f) + 6.0f;
        for (int k = 0; k < n; k++) {
                float side = frand() < 0.5f ? -1.0f : 1.0f;
                float up = (60.0f + 200.0f * c) * (0.3f + 0.7f * frand());
                float tan = side * (80.0f + 260.0f * c) * (0.2f + 0.8f * frand()) + 0.3f * vt;
                float life = 0.4f + (0.3f + 0.5f * c) * frand();
                speck(BOTTOM, px, py, (frand() - 0.5f) * sp, up, tan, life, frand() < 0.4f + 0.4f * c);
        }
}

void wallfx_step(const wallfx_marble_t *m, float dt, float W, float H)
{
        float dist[4] = {
                m->x + m->x0, W - (m->x + m->x1),
                m->y + m->y0, H - (m->y + m->y1),
        };
        float vn[4] = { -m->vx, m->vx, -m->vy, m->vy };         /* into the wall */
        /* Along it, after the bounce: wedged in a corner, the speed into
         * the other wall isn't sliding along this one */
        float vt[4] = { m->ovy, m->ovy, m->ovx, m->ovx };
        /* Where the cursor meets each wall: middle of its footprint */
        float mid_x = m->x + (m->x0 + m->x1) * 0.5f;
        float mid_y = m->y + (m->y0 + m->y1) * 0.5f;
        float len_x = m->x1 - m->x0, len_y = m->y1 - m->y0;
        float cpx[4] = { 0.5f, W - 1.5f, mid_x, mid_x };
        float cpy[4] = { mid_y, mid_y, 0.5f, H - 1.5f };
        float span[4] = { len_y, len_y, len_x, len_x };

        float dust_amt = tune.dust;
        float hit = 0, scrape = 0, crash = 0;
        float thunk_full = m->lander ? LANDER_THUNK_FULL : THUNK_FULL;

        for (int i = 0; i < 4; i++) {
                wall_t *w = &walls[i];
                if (w->cooldown > 0)
                        w->cooldown -= dt;
                if (dist[i] > REARM_DIST)
                        w->armed = 1;

                if (dist[i] <= HIT_EPS && vn[i] > 0) {
                        if (w->armed && vn[i] >= THUNK_MIN && w->cooldown <= 0) {
                                float h = clamp01((vn[i] - THUNK_MIN) / (thunk_full - THUNK_MIN));
                                if (m->lander && i == BOTTOM && vn[i] >= HARD_LANDING) {
                                        float c = clamp01((vn[i] - HARD_LANDING) / (CRASH_FULL - HARD_LANDING));
                                        crash = c + 1e-3f;
                                        crash_dust(cpx[i], cpy[i], span[i], c, vt[i], dust_amt);
                                }
                                if (h + 1e-3f > hit)
                                        hit = h + 1e-3f;
                                w->cooldown = COOLDOWN;

                                int n = (int)((PUFF_BASE + PUFF_HARD * h) * dust_amt + 0.5f);
                                float sp = fminf(span[i], 14.0f) * 0.8f;
                                for (int k = 0; k < n; k++) {
                                        float out = (35.0f + 170.0f * h) * (0.35f + 0.65f * frand());
                                        float tan = (frand() * 2 - 1) * 0.9f * out + 0.3f * vt[i];
                                        float life = 0.25f + (0.2f + 0.35f * h) * frand();
                                        speck(i, cpx[i], cpy[i], (frand() - 0.5f) * sp, out, tan, life,
                                              frand() < 0.25f + 0.5f * h);
                                }
                        }
                        w->armed = 0;
                }

                if (dist[i] <= TOUCH_DIST)
                        w->hold = TOUCH_HOLD;
                else if (w->hold > 0)
                        w->hold -= dt;

                if (w->hold > 0) {
                        float a = fabsf(vt[i]);
                        float lvl = powf(clamp01((a - SCRAPE_MIN) / (SCRAPE_FULL - SCRAPE_MIN)), 0.6f);
                        if (lvl > scrape)
                                scrape = lvl;
                        /* A trickle of dust shed behind the cursor */
                        w->dust_acc += dt * SCRAPE_RATE * lvl * dust_amt;
                        float back = vt[i] > 0 ? -1.0f : 1.0f;
                        while (w->dust_acc >= 1.0f) {
                                w->dust_acc -= 1.0f;
                                float out = 8.0f + 40.0f * lvl * frand();
                                float tan = 0.15f * vt[i] * frand() + (frand() - 0.5f) * 20.0f;
                                speck(i, cpx[i], cpy[i], back * frand() * 4.0f, out, tan,
                                      0.15f + 0.25f * frand(), frand() < 0.15f * lvl);
                        }
                } else {
                        w->dust_acc = 0;
                }
        }

        sfx_set_volume(tune.sfx_volume);
        if (crash > 0)
                sfx_thunk_heavy(crash - 1e-3f);
        else if (hit > 0)
                sfx_thunk(hit - 1e-3f);
        sfx_rustle(scrape);
        dust_step(dt, m->gx, m->gy, W, H);
}
