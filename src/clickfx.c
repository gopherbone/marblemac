/* Mouse button -> thump, release tap and drag rustle.
 *
 *  - Press: one thump on each up->down edge of the button, so holding it
 *    never repeats.  A short lockout after each thump stops a bouncy or
 *    hammered button from machine-gunning.
 *
 *  - Release: a much quieter tap on down->up, but only if the button was
 *    held long enough for the thump to have mostly died away; a quick
 *    click is just the thump.
 *
 *  - Drag: while the button is held, the cursor's speed sets the drag
 *    rustle's level (with a dead zone so a resting, jittering marble is
 *    silent).  The audio side smooths it into a fade in and out.
 */

#include <math.h>
#include "clickfx.h"
#include "sfx.h"
#include "tuning.h"

#define LOCKOUT         0.07f   /* s: minimum time between thumps */
#define UNCLICK_MIN     0.12f   /* s held before a release makes a sound */
#define DRAG_MIN        15.0f   /* px/s: slower is silent */
#define DRAG_FULL       900.0f  /* px/s for the loudest drag */

static int was_down;
static float held, since_thump = LOCKOUT;

void clickfx_step(int down, float vx, float vy, float dt)
{
        down = down != 0;
        sfx_set_click_volume(tune.click_volume);
        if (since_thump < LOCKOUT)
                since_thump += dt;

        if (down && !was_down) {
                if (since_thump >= LOCKOUT) {
                        sfx_click();
                        since_thump = 0;
                }
                held = 0;
        } else if (!down && was_down) {
                if (held >= UNCLICK_MIN)
                        sfx_unclick();
        } else if (down) {
                held += dt;
        }
        was_down = down;

        float level = 0;
        if (down) {
                float sp = sqrtf(vx * vx + vy * vy);
                float l = (sp - DRAG_MIN) * (1.0f / (DRAG_FULL - DRAG_MIN));
                level = l <= 0 ? 0 : l >= 1 ? 1 : powf(l, 0.6f);
        }
        sfx_drag(level);
}
