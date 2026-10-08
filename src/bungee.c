/* Bungee: an elastic rope from an anchor on the Mac screen to the cursor.
 *
 * The rope only pulls when stretched past its rest length L: a spring
 * k(d - L) along the rope, plus damping of the speed along the rope.
 * The damping is integrated implicitly, and the pull is capped so one
 * step can never fling the marble back past the rest length, which keeps
 * it stable at any frame rate we'll see (dt up to 0.1s).
 *
 * Drawn as a black line with a white halo so it shows on any background;
 * slack rope sags downhill (a quadratic bezier), taut rope is straight.
 *
 * While anchored, the arrow hangs from its tip (where the rope ties on) as
 * a damped physical pendulum; see bungee_arrow().
 */

#include <math.h>
#include <stdlib.h>
#include "bungee.h"
#include "tuning.h"

#define START_SLACK     24.0f   /* rest length when dropped, px */
#define MAX_LEN         400.0f
#define REEL_PER_DEG    0.6f    /* px of rope per degree of crank */
#define REEL_RATE       160.0f  /* px/s with Up/Right held */

static PlaydateAPI *pd;
static int active;
static float anc_x, anc_y, rest;
static float sag_gx, sag_gy = 1;

void bungee_init(PlaydateAPI *playdate)
{
        pd = playdate;
}

int bungee_active(void)
{
        return active;
}

void bungee_input(PDButtons cur, PDButtons pushed, int panel_open, float dt,
                  float px, float py, int enabled)
{
        /* The panel reads the crank itself while it's open */
        if (panel_open)
                return;
        float crank = pd ? pd->system->getCrankChange() : 0;
        if (!enabled) {
                active = 0;
                return;
        }

        if (pushed & kButtonDown) {
                active = !active;
                if (active) {
                        anc_x = px;
                        anc_y = py;
                        rest = START_SLACK;
                }
        } else if (pushed & kButtonLeft) {
                active = 0;
        }
        if (!active)
                return;

        /* Forward cranking reels in, like a fishing reel */
        float dl = -crank * REEL_PER_DEG;
        if (cur & kButtonUp)
                dl -= REEL_RATE * dt;
        if (cur & kButtonRight)
                dl += REEL_RATE * dt;
        if (dl < 0) {
                /* Reeling in takes up any slack first */
                float dx = px - anc_x, dy = py - anc_y;
                float d = sqrtf(dx * dx + dy * dy);
                if (rest > d)
                        rest = d;
        }
        rest = fmaxf(0, fminf(MAX_LEN, rest + dl));
}

void bungee_apply(float px, float py, float *vx, float *vy, float gx, float gy, float dt)
{
        float gl = sqrtf(gx * gx + gy * gy);
        if (gl > 1.0f) {
                sag_gx = gx / gl;
                sag_gy = gy / gl;
        }
        if (!active || dt <= 0)
                return;

        float dx = px - anc_x, dy = py - anc_y;
        float d = sqrtf(dx * dx + dy * dy);
        float x = d - rest;
        if (x <= 0 || d < 1e-3f)
                return;
        float ux = dx / d, uy = dy / d;
        float vr = *vx * ux + *vy * uy;         /* outward speed */
        /* Spring explicit (softened if a long frame would make it
         * unstable: needs k dt^2 < 4), damping implicit.
         */
        float k = fminf(tune.rope_k, 3.0f / (dt * dt));
        float nvr = (vr - dt * k * x) / (1 + dt * tune.rope_damp);
        /* Never pull it further in than back to the rest length this step */
        nvr = fmaxf(nvr, fminf(vr, -x / dt));
        *vx += (nvr - vr) * ux;
        *vy += (nvr - vr) * uy;
}

////////////////////////////////////////////////////////////////////////////////
// The arrow swinging from its tip

/* theta is the direction the tail hangs (the tip points the other way),
 * with the tail's weight an effective length SWING_LEN from the pivot.  In
 * the tip's frame the tail feels g_eff = tilt - tip acceleration, so
 * hauling the cursor about swings it, and in free flight it floats:
 *
 *      theta'' = (u x g_eff) / SWING_LEN - swing_damp * theta'
 *
 * with u = (cos theta, sin theta), i.e. -(|g|/len) sin(theta - downhill).
 * No atan2 of g, so near-zero gravity just leaves it coasting.
 * Semi-implicit Euler, damping implicit, in substeps of <= 1/120 s.
 */
#define SWING_LEN       12.0f   /* px, pivot to the tail's centre of mass */
#define SWING_MAX_G     8000.0f /* clamp on g_eff, px/s^2 */
#define SWING_MAX_W     40.0f   /* clamp on spin, rad/s */
#define SWING_MAX_DT    0.1f
#define SWING_SUBSTEP   (1.0f / 120)
#define RELEASE_T       0.5f    /* s to fade steering back in on letting go */
#define RELEASE_SPIN    4.0f    /* decay of leftover spin after letting go, 1/s */

static int swing_on;            /* pendulum state valid (was anchored) */
static float sw_th, sw_w;       /* tail angle, spin */
static float sw_pvx, sw_pvy;    /* last velocity, for the tip's acceleration */
static float sw_out;            /* last angle returned */
static float sw_rel = RELEASE_T;        /* time since letting go */

static float wrap(float a)
{
        return remainderf(a, 2 * (float)M_PI);
}

float bungee_arrow(float angle, float vx, float vy, float gx, float gy, float dt)
{
        if (!(dt > 0))
                return swing_on || sw_rel < RELEASE_T ? sw_out : angle;
        dt = fminf(dt, SWING_MAX_DT);

        if (active) {
                if (!swing_on) {
                        /* Grabbed: start hanging as it is, still */
                        swing_on = 1;
                        sw_th = wrap(angle + (float)M_PI);
                        sw_w = 0;
                        sw_pvx = vx;
                        sw_pvy = vy;
                }
                /* Effective gravity in the tip's frame */
                float ex = gx - (vx - sw_pvx) / dt;
                float ey = gy - (vy - sw_pvy) / dt;
                sw_pvx = vx;
                sw_pvy = vy;
                float el = sqrtf(ex * ex + ey * ey);
                if (el > SWING_MAX_G) {
                        ex *= SWING_MAX_G / el;
                        ey *= SWING_MAX_G / el;
                }
                int n = (int)ceilf(dt / SWING_SUBSTEP);
                float h = dt / n;
                float damp = 1 / (1 + h * tune.swing_damp);
                for (int i = 0; i < n; i++) {
                        float acc = (cosf(sw_th) * ey - sinf(sw_th) * ex) / SWING_LEN;
                        sw_w = (sw_w + h * acc) * damp;
                        sw_w = fmaxf(-SWING_MAX_W, fminf(SWING_MAX_W, sw_w));
                        sw_th = wrap(sw_th + h * sw_w);
                }
                sw_rel = 0;
                sw_out = wrap(sw_th + (float)M_PI);
                return sw_out;
        }

        if (swing_on) {
                /* Let go: angle was steered from our last answer */
                swing_on = 0;
                sw_rel = 0;
        }
        if (sw_rel >= RELEASE_T)
                return angle;

        /* Fade the steering back in (smoothstep), and let the spin we had
         * on the rope coast out, so the arrow neither snaps nor stops dead.
         */
        sw_rel += dt;
        float t = fminf(1, sw_rel / RELEASE_T);
        float w = t * t * (3 - 2 * t);
        sw_w *= expf(-RELEASE_SPIN * dt);
        sw_out = wrap(sw_out + w * wrap(angle - sw_out) + sw_w * dt);
        return sw_out;
}

////////////////////////////////////////////////////////////////////////////////
// Drawing

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

/* pass 0: white 3x3 halo, pass 1: black core */
static void line(int x0, int y0, int x1, int y1, int pass)
{
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;
        for (;;) {
                if (pass == 0) {
                        for (int j = -1; j <= 1; j++)
                                for (int i = -1; i <= 1; i++)
                                        plot(x0 + i, y0 + j, 1);
                } else {
                        plot(x0, y0, 0);
                }
                if (x0 == x1 && y0 == y1)
                        break;
                int e2 = 2 * err;
                if (e2 >= dy) {
                        err += dy;
                        x0 += sx;
                }
                if (e2 <= dx) {
                        err += dx;
                        y0 += sy;
                }
        }
}

/* An eye bolt: # black, . white, space untouched */
static const char *const anchor_pic[9] = {
        "  .....  ",
        " .#####. ",
        ".##...##.",
        ".#.....#.",
        ".#..#..#.",
        ".#.....#.",
        ".##...##.",
        " .#####. ",
        "  .....  ",
};

void bungee_draw(uint8_t *frame, int rowbytes, int width, int height,
                 int cam_x, int cam_y, int px, int py)
{
        if (!active)
                return;
        fb = frame;
        fb_rb = rowbytes;
        fb_w = width;
        fb_h = height;

        float x0 = anc_x - cam_x, y0 = anc_y - cam_y;
        float x2 = (float)(px - cam_x), y2 = (float)(py - cam_y);
        float dx = x2 - x0, dy = y2 - y0;
        float d = sqrtf(dx * dx + dy * dy);
        float slack = rest - d;

        /* Control point: straight when taut, else pushed downhill (with a
         * little sideways bulge so rope hanging along the chord still
         * reads as a loop).  A parabola of chord d and depth h is about
         * d + 8h^2/3d long; the slack^2/4 term makes a rope with both
         * ends together hang half its length.
         */
        float cx1 = (x0 + x2) / 2, cy1 = (y0 + y2) / 2;
        if (slack > 0.5f) {
                float h = sqrtf(3 * d * slack / 8 + slack * slack / 4);
                h = fminf(h, rest / 2);
                float sx = sag_gx, sy = sag_gy;
                if (d > 1) {
                        float pxn = -dy / d, pyn = dx / d;
                        if (pxn * sx + pyn * sy < 0) {
                                pxn = -pxn;
                                pyn = -pyn;
                        }
                        sx += 0.35f * pxn;
                        sy += 0.35f * pyn;
                        float l = sqrtf(sx * sx + sy * sy);
                        sx /= l;
                        sy /= l;
                }
                cx1 += 2 * h * sx;
                cy1 += 2 * h * sy;
        }

        int n = slack > 0.5f ? (int)((d + 2 * (rest - d)) / 6) : 1;
        if (n < 1)
                n = 1;
        if (n > 32)
                n = 32;
        for (int pass = 0; pass < 2; pass++) {
                int lx = (int)lrintf(x0), ly = (int)lrintf(y0);
                for (int i = 1; i <= n; i++) {
                        float t = (float)i / n, u = 1 - t;
                        float bx = u * u * x0 + 2 * u * t * cx1 + t * t * x2;
                        float by = u * u * y0 + 2 * u * t * cy1 + t * t * y2;
                        int ix = (int)lrintf(bx), iy = (int)lrintf(by);
                        line(lx, ly, ix, iy, pass);
                        lx = ix;
                        ly = iy;
                }
        }

        int ax = (int)lrintf(x0) - 4, ay = (int)lrintf(y0) - 4;
        for (int j = 0; j < 9; j++)
                for (int i = 0; i < 9; i++) {
                        char c = anchor_pic[j][i];
                        if (c != ' ')
                                plot(ax + i, ay + j, c == '.');
                }
}
