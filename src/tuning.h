#ifndef TUNING_H
#define TUNING_H

#include "pd_api.h"

/* Live-tweakable marble physics.  B opens a panel: up/down picks a value,
 * left/right (or the crank) changes it.  Saved to the Data folder.
 */
typedef struct {
        float tilt_gain;        /* px/s^2 per g of tilt */
        float drag;             /* viscous drag, 1/s */
        float roll_friction;    /* constant decel, px/s^2 */
        float wall_bounce;      /* fraction of speed kept off a wall */
        float max_speed;        /* px/s */
        float dead_zone;        /* g of tilt ignored */
        float turn_rate;        /* how fast the arrow swings round, 1/s */
        float cam_follow;       /* how fast the view catches up, 1/s */
        float cam_lookahead;    /* how far ahead the view leads, s */
        float sfx_volume;       /* wall sounds, 0..1 */
        float dust;             /* amount of wall dust, 0 = none */
        float click_volume;     /* mouse button and drag sounds, 0..1 */
        float rope_k;           /* bungee stiffness, px/s^2 per px of stretch */
        float rope_damp;        /* bungee damping along the rope, 1/s */
        float swing_damp;       /* damping of the arrow swinging on the bungee, 1/s */
} tuning_t;

extern tuning_t tune;

void    tuning_init(PlaydateAPI *pd);
/* Handle input; returns 1 while the panel is open (it eats the d-pad) */
int     tuning_update(PDButtons cur, PDButtons pushed, float dt);
void    tuning_draw(void);
void    tuning_save(void);

#endif
