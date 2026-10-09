#ifndef LANDER_H
#define LANDER_H

#include <stdint.h>

/* The cursor as a lunar lander: constant gravity down the screen, a main
 * engine that pushes along the nose, and the nose steered by the crank
 * (or the d-pad with the crank docked).  Pure physics and drawing, no
 * Playdate API, so tools/lander_test.c can step it on the host.
 *
 * Angles are radians in screen coordinates (y down): 0 = right,
 * -pi/2 = up, increasing clockwise.  The heading is where the nose (the
 * arrow's tip, its hotspot) points.
 */

typedef struct {
        float gravity;          /* px/s^2, straight down */
        float thrust;           /* px/s^2 along the nose */
        float turn;             /* d-pad rotation, deg/s */
        float drag;             /* 1/s */
        float ground_friction;  /* px/s^2 sliding along the floor */
        float max_speed;        /* px/s */
        float wall_bounce;      /* fraction of speed kept off a wall */
} lander_params_t;

typedef struct {
        int steer;              /* 0: hold the heading (the panel has the controls) */
        int docked;             /* crank docked: d-pad steers */
        float crank_deg;        /* crank angle, 0 = up, clockwise */
        int rot;                /* d-pad: -1 left, +1 right */
} lander_steer_t;

/* Heading for crank angle deg (0 = up, clockwise) */
float   lander_crank_heading(float deg);

/* Turn the heading.  Undocked, it chases the crank's angle quickly (fast
 * enough to feel absolute, but undocking or closing the panel never
 * snaps it).
 */
float   lander_steer(float heading, const lander_steer_t *in, const lander_params_t *p, float dt);

/* Gravity plus the engine, as an acceleration, px/s^2 */
void    lander_accel(float heading, int thrust, const lander_params_t *p, float *ax, float *ay);

/* After the accel (and any rope pull) has gone into (vx, vy): drag,
 * ground friction, the speed cap, move, and keep the whole cursor on the
 * screen.  (x0..x1, y0..y1) is the cursor's footprint around the hotspot
 * (as cursor_extent()), w x h the screen.  in_v* get the velocity from
 * just before the walls bounced it (for the wall effects).
 */
void    lander_move(float *x, float *y, float *vx, float *vy, const lander_params_t *p,
                    float x0, float x1, float y0, float y1, float w, float h,
                    float dt, float *in_vx, float *in_vy);

/* Exhaust: advance the flame's flicker and, if the jet is close enough to
 * a wall to wash over it, kick up dust there.  (x, y) is the hotspot.
 */
void    lander_flame_step(int thrust, float x, float y, float heading, float w, float h,
                          float dust, float dt);
/* Draw the flame out of the back of the arrow, under the cursor, into a
 * 1-bit frame (bit set = white).  (x, y) is the hotspot in frame pixels.
 */
void    lander_flame_draw(uint8_t *frame, int rowbytes, int width, int height,
                          int x, int y, float heading);

#endif
