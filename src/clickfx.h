#ifndef CLICKFX_H
#define CLICKFX_H

/* Sounds for the mouse button: a thump on press, a faint tap on release,
 * and a low rustle while dragging.  Call once per frame with the button
 * state and the cursor's velocity (px/s).
 */
void    clickfx_step(int down, float vx, float vy, float dt);

#endif
