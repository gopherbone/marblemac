#ifndef BUNGEE_H
#define BUNGEE_H

#include <stdint.h>
#include "pd_api.h"

/* A bungee cord for the cursor.  D-pad Down drops an anchor where the
 * cursor is (Down again, or Left, lets go); the crank reels the rope in
 * (forward) and out (backward), as do Up and Right held.  The anchor is
 * in Mac screen pixels, so it stays put on the Mac's screen as the view
 * scrolls.  Stiffness and damping are in the tuning panel.
 *
 * In lander mode (Up thrusts, the crank and Left/Right steer) it's all on
 * Down: Down drops the anchor, a tap of Down lets go, and with Down held
 * Up reels in and Right pays out.
 */

void    bungee_init(PlaydateAPI *pd);
/* Handle input.  panel_open = the tuning panel has the d-pad and crank
 * this frame, so leave them alone.  (px, py) is the marble.  enabled = 0
 * drops the rope (Mac not ready).  lander = use lander mode's buttons.
 */
void    bungee_input(PDButtons cur, PDButtons pushed, int panel_open, float dt,
                     float px, float py, int enabled, int lander);
/* Apply the rope's pull to the marble's velocity, after the tilt has
 * been added and before drag/friction/position.  (gx, gy) is the tilt
 * acceleration in px/s^2, remembered for drawing the sag.
 */
void    bungee_apply(float px, float py, float *vx, float *vy, float gx, float gy, float dt);
/* Draw the rope from the anchor to the hotspot (px, py), and the anchor,
 * into a 1-bit Playdate frame (bit set = white), view top-left at Mac
 * pixel (cam_x, cam_y).  Call before drawing the cursor.
 */
void    bungee_draw(uint8_t *frame, int rowbytes, int width, int height,
                    int cam_x, int cam_y, int px, int py);

int     bungee_active(void);

/* The arrow's direction (where its tip points, radians) given the one the
 * normal "point where it's rolling" steering just produced.  While
 * anchored, the arrow hangs from its tip like a weighted pendulum and
 * swings under the tilt (gx, gy), less the tip's own acceleration
 * (worked out from the marble's velocity (vx, vy) frame to frame).  For a
 * moment after letting go, the steering is faded back in and any leftover
 * spin coasts out.  Otherwise returns angle unchanged.  Call once per
 * step, after the steering and before the cursor's extent is used.
 */
float   bungee_arrow(float angle, float vx, float vy, float gx, float gy, float dt);
/* Forget any swing (the cursor mode changed: the next bungee_arrow()
 * starts afresh from the angle it's given) */
void    bungee_swing_reset(void);

#endif
