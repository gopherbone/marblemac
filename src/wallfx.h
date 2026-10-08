#ifndef WALLFX_H
#define WALLFX_H

/* Sound and dust for the marble meeting the edges of the Mac screen.
 * Fed once per physics step, after the walls have clamped the marble.
 */
typedef struct {
        float x, y;             /* hotspot, after clamping to the walls */
        float vx, vy;           /* velocity before the wall bounce, px/s */
        float ovx, ovy;         /* ...and after it */
        float x0, x1, y0, y1;   /* cursor footprint around the hotspot */
        float gx, gy;           /* tilt acceleration, px/s^2 */
} wallfx_marble_t;

void    wallfx_step(const wallfx_marble_t *m, float dt, float screen_w, float screen_h);

#endif
