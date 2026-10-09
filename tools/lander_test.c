/* Host test for the lander physics (src/lander.c), stepped the way
 * main.c's step_lander() does, with the real arrow's footprint:
 *
 *  - random flying (thrust, crank, d-pad) at frame times up to 0.1s: the
 *    whole arrow stays on the 512x342 screen, nothing goes NaN, speed
 *    stays under the cap;
 *  - it falls, lands and comes to rest on the floor;
 *  - full thrust pointing up climbs, and the landing speeds are sane;
 *  - the crank sets the heading (0 = up) without snapping;
 *  - draws the exhaust as ASCII for a look.
 *
 * cc -O2 -Isrc tools/lander_test.c src/lander.c src/cursor.c src/dust.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/cursor.h"
#include "../src/lander.h"

#define W 512
#define H 342

static const mac_cursor_t arrow = {
        { 0x0000, 0x4000, 0x6000, 0x7000, 0x7800, 0x7C00, 0x7E00, 0x7F00,
          0x7F80, 0x7C00, 0x6C00, 0x4600, 0x0600, 0x0300, 0x0300, 0x0000 },
        { 0xC000, 0xE000, 0xF000, 0xF800, 0xFC00, 0xFE00, 0xFF00, 0xFF80,
          0xFFC0, 0xFFE0, 0xFE00, 0xEF00, 0xCF00, 0x8780, 0x0780, 0x0380 },
        1, 1
};

/* tuning.c's defaults */
static const lander_params_t P = {
        .gravity = 160, .thrust = 420, .turn = 270, .drag = 0.15f,
        .ground_friction = 300, .max_speed = 2400, .wall_bounce = 0.15f,
};

static float x, y, vx, vy, heading;
static float e0, e1, e2, e3;
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void step(int thrust, const lander_steer_t *st, float dt)
{
        heading = lander_steer(heading, st, &P, dt);
        float ax, ay;
        lander_accel(heading, thrust, &P, &ax, &ay);
        vx += ax * dt;
        vy += ay * dt;
        cursor_extent(&arrow, heading - CURSOR_ARROW_ANGLE, &e0, &e1, &e2, &e3);
        float ivx, ivy;
        lander_move(&x, &y, &vx, &vy, &P, e0, e1, e2, e3, W, H, dt, &ivx, &ivy);
        lander_flame_step(thrust, x, y, heading, W, H, 1, dt);
}

static int on_screen(void)
{
        /* cursor_extent is pixel corners around the hotspot pixel's centre */
        const float eps = 1e-3f;
        return x + e0 >= -eps && x + e1 <= W + eps && y + e2 >= -eps && y + e3 <= H + eps;
}

static float frnd(void)
{
        return rand() / (float)RAND_MAX;
}

static void random_flight(float dt_max, int frames)
{
        x = W / 2;
        y = H / 2;
        vx = vy = 0;
        heading = -(float)M_PI / 2;
        float max_sp = 0;
        int thrust = 0;
        lander_steer_t st = { .steer = 1, .docked = 1 };
        for (int i = 0; i < frames; i++) {
                if (rand() % 20 == 0)
                        thrust = rand() % 3 != 0;
                if (rand() % 30 == 0) {
                        st.docked = rand() % 2;
                        st.rot = rand() % 3 - 1;
                }
                if (!st.docked)
                        st.crank_deg = fmodf(st.crank_deg + (frnd() - 0.5f) * 60 + 360, 360);
                st.steer = rand() % 50 != 0;
                float dt = dt_max * (0.05f + 0.95f * frnd());
                step(thrust, &st, dt);
                float sp = sqrtf(vx * vx + vy * vy);
                if (sp > max_sp)
                        max_sp = sp;
                if (!isfinite(x) || !isfinite(y) || !isfinite(vx) || !isfinite(vy) || !isfinite(heading)) {
                        CHECK(0, "dt<=%.3f frame %d: not finite", dt_max, i);
                        return;
                }
                if (!on_screen()) {
                        CHECK(0, "dt<=%.3f frame %d: off screen at (%.2f,%.2f) ext x %.2f..%.2f y %.2f..%.2f",
                              dt_max, i, x, y, x + e0, x + e1, y + e2, y + e3);
                        return;
                }
                CHECK(sp <= P.max_speed * 1.001f, "speed %.0f over the cap", sp);
        }
        printf("random flight, dt up to %.3fs, %d frames: ok, top speed %.0f px/s\n", dt_max, frames, max_sp);
}

static void fall_and_land(float dt)
{
        x = W / 2;
        y = 20;
        vx = 120;
        vy = 0;
        heading = -(float)M_PI / 2;
        lander_steer_t st = { .steer = 1, .docked = 1 };
        float impact = 0, t = 0;
        int landed_at = -1;
        for (int i = 0; t < 8; i++, t += dt) {
                float pvy = vy;
                step(0, &st, dt);
                if (pvy > 0 && vy <= 0 && impact == 0)
                        impact = pvy;
                if (landed_at < 0 && fabsf(vx) < 1e-6f && fabsf(vy) < 20 && y >= H - e3 - 0.01f)
                        landed_at = i;
                CHECK(on_screen(), "fall dt %.3f: off screen", dt);
        }
        printf("fall from the top at dt %.3f: hit the floor at %.0f px/s, ", dt, impact);
        printf("at rest %s (x %.1f, vy %.2f)\n", landed_at >= 0 ? "yes" : "NO", x, vy);
        CHECK(landed_at >= 0, "never came to rest on the floor");
        CHECK(impact > 200 && impact < 450, "free-fall impact %.0f out of the expected range", impact);
}

static void climb(void)
{
        x = W / 2;
        y = H - 30;
        vx = vy = 0;
        heading = -(float)M_PI / 2;
        lander_steer_t st = { .steer = 1, .docked = 1 };
        float y0 = y;
        for (int i = 0; i < 30; i++)
                step(1, &st, 1 / 50.0f);
        printf("full thrust up for 0.6s: climbed %.0f px, now %.0f px/s up\n", y0 - y, -vy);
        CHECK(y < y0 - 30 && vy < 0, "can't climb");
}

static void crank(void)
{
        heading = 0;
        lander_steer_t st = { .steer = 1, .docked = 0, .crank_deg = 0 };
        x = W / 2;
        y = H / 2;
        vx = vy = 0;
        float jump = 0, prev = heading;
        for (int i = 0; i < 20; i++) {
                step(0, &st, 1 / 50.0f);
                jump = fmaxf(jump, fabsf(remainderf(heading - prev, 2 * (float)M_PI)));
                prev = heading;
        }
        printf("crank 0 deg -> heading %.1f deg (want -90), biggest step %.1f deg/frame\n",
               heading * 180 / M_PI, jump * 180 / M_PI);
        CHECK(fabsf(heading + (float)M_PI / 2) < 0.01f, "crank 0 isn't up");
        CHECK(jump < 0.7f, "heading snapped");
        st.crank_deg = 90;
        for (int i = 0; i < 30; i++)
                step(0, &st, 1 / 50.0f);
        CHECK(fabsf(heading) < 0.01f, "crank 90 isn't right (%.2f)", heading);
        st.crank_deg = 180;
        for (int i = 0; i < 30; i++)
                step(0, &st, 0.1f);
        CHECK(fabsf(heading - (float)M_PI / 2) < 0.01f, "crank 180 isn't down at dt 0.1 (%.2f)", heading);
        /* docked: right turns clockwise at the set rate */
        st.docked = 1;
        st.rot = 1;
        heading = -(float)M_PI / 2;
        for (int i = 0; i < 50; i++)
                heading = lander_steer(heading, &st, &P, 1 / 50.0f);
        printf("d-pad right for 1s: heading %.1f deg (want %.0f)\n", heading * 180 / M_PI, remainderf(-90 + P.turn, 360));
        CHECK(fabsf(remainderf(heading * 180 / (float)M_PI - (-90 + P.turn), 360)) < 0.5f, "turn rate");
}

static void draw_flame(void)
{
        enum { FW = 64, FH = 48, S = FW / 8 };
        float dirs[] = { -90, 0, 135 };
        for (unsigned k = 0; k < sizeof dirs / sizeof *dirs; k++) {
                float h = dirs[k] * (float)M_PI / 180;
                uint8_t fw[S * FH], fb[S * FH];
                for (int i = 0; i < 4; i++)
                        lander_flame_step(1, 0, 0, h, 10000, 10000, 0, 0.05f);
                memset(fw, 0xff, sizeof fw);
                memset(fb, 0x00, sizeof fb);
                int hx = FW / 2 + (int)(cosf(h) * 10), hy = FH / 2 + (int)(sinf(h) * 10);
                for (int pass = 0; pass < 2; pass++) {
                        uint8_t *f = pass ? fb : fw;
                        lander_flame_draw(f, S, FW, FH, hx, hy, h);
                        cursor_draw(f, S, FW, FH, &arrow, hx, hy, h - CURSOR_ARROW_ANGLE);
                }
                printf("nose %.0f deg, thrusting ('#' black, 'o' white, '.' unchanged)\n", dirs[k]);
                for (int yy = 0; yy < FH; yy++) {
                        for (int xx = 0; xx < FW; xx++) {
                                int a = (fw[yy * S + xx / 8] >> (7 - (xx & 7))) & 1;
                                int b = (fb[yy * S + xx / 8] >> (7 - (xx & 7))) & 1;
                                putchar(xx == hx && yy == hy ? '+' : (a && b) ? 'o' : (!a && !b) ? '#' : '.');
                        }
                        putchar('\n');
                }
        }
}

int main(int argc, char **argv)
{
        srand(1);
        float dts[] = { 1 / 50.0f, 1 / 30.0f, 0.05f, 0.1f };
        for (unsigned i = 0; i < sizeof dts / sizeof *dts; i++)
                random_flight(dts[i], 200000);
        for (unsigned i = 0; i < sizeof dts / sizeof *dts; i++)
                fall_and_land(dts[i]);
        climb();
        crank();
        if (argc > 1 && !strcmp(argv[1], "-d"))
                draw_flame();
        printf(fails ? "%d FAILED\n" : "all ok\n", fails);
        return fails != 0;
}
