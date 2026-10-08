#ifndef CURSOR_H
#define CURSOR_H

#include <stdint.h>

/* A classic QuickDraw 'Cursor' record, as found at TheCrsr (0x844) in
 * Mac low memory: 16x16 1bpp data + mask, then hotspot (v, h).
 */
typedef struct {
        uint16_t data[16];
        uint16_t mask[16];
        int16_t  hot_v;
        int16_t  hot_h;
} mac_cursor_t;

/* The rest direction of the standard arrow (where its tip points), in
 * radians, screen coordinates (y down).
 */
extern const float CURSOR_ARROW_ANGLE;

/* True if the cursor looks like a pointer (hotspot at its top-left
 * corner), i.e. something that makes sense to rotate.
 */
int     cursor_is_pointer(const mac_cursor_t *c);

/* Bounding box of the cursor's opaque (mask) pixels when drawn rotated,
 * in pixels relative to the hotspot pixel.
 */
void    cursor_extent(const mac_cursor_t *c, float rotation,
                      float *minx, float *maxx, float *miny, float *maxy);

/* Draw cursor c into a 1bpp framebuffer where a set bit is WHITE
 * (Playdate convention), with its hotspot at (x, y), rotated by
 * 'rotation' radians around the hotspot.  A rotation of 0 draws the
 * cursor exactly as the Mac would (including XOR pixels).
 */
void    cursor_draw(uint8_t *frame, int stride, int width, int height,
                    const mac_cursor_t *c, int x, int y, float rotation);

#endif
