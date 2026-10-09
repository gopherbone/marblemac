#ifndef SFX_H
#define SFX_H

#include "pd_api.h"

/* Procedural sound effects for the marble hitting and scraping along the
 * edges of the Mac screen, and for the mouse button.  Everything is synthesised in one audio
 * callback; the game thread only posts events and targets.
 */

void    sfx_init(PlaydateAPI *pd);
/* Master volume, 0..1 */
void    sfx_set_volume(float vol);
/* A thunk against a wall.  hardness 0..1 (0 = barely, 1 = slammed) */
void    sfx_thunk(float hardness);
/* The scraping voice's target level, 0..1.  Call every frame: if the
 * game stops calling it (paused, Mac reset) the scrape fades out by itself.
 */
void    sfx_rustle(float level);

/* A hard landing: heavier and longer than any thunk */
void    sfx_thunk_heavy(float hardness);
/* The lander's engine, 0..1; call every frame, as sfx_rustle().  Played
 * at the wall sound volume. */
void    sfx_thrust(float level);

/* Mouse button sounds: their own volume, 0..1 */
void    sfx_set_click_volume(float vol);
/* A soft, low thump for the button going down... */
void    sfx_click(void);
/* ...and a much quieter tap for it coming back up */
void    sfx_unclick(void);
/* The drag rustle's target level, 0..1; call every frame, as sfx_rustle() */
void    sfx_drag(float level);

#endif
