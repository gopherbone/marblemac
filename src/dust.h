#ifndef DUST_H
#define DUST_H

#include <stdint.h>

/* 1-bit dust specks kicked up where the marble meets a wall.  Positions
 * are in Mac screen pixels, so the dust stays put on the Mac's screen
 * however the view scrolls.
 */

/* Spawn one speck at (x, y) moving at (vx, vy) px/s, living `life` s.
 * big = starts as a 2x2 speck rather than a single pixel.
 */
void    dust_spawn(float x, float y, float vx, float vy, float life, int big);
/* Move and age the dust.  (gx, gy) is the board's tilt as an acceleration
 * in px/s^2: dust drifts downhill a little.  Dust stops at the screen
 * edges (w x h).
 */
void    dust_step(float dt, float gx, float gy, float w, float h);
/* Draw into a 1-bit Playdate frame (bit set = white), with the view's
 * top-left at Mac pixel (cam_x, cam_y).
 */
void    dust_draw(uint8_t *frame, int rowbytes, int width, int height, int cam_x, int cam_y);

#endif
