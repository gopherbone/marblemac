#ifndef TUNING_H
#define TUNING_H

#include "pd_api.h"

/* Live-tweakable physics.  B opens a panel: up/down picks a value,
 * left/right (or the crank) changes it.  Saved to the Data folder.  The
 * first row picks the cursor mode; rows for the other mode are hidden.
 */
typedef struct {
        float mode;             /* MODE_MARBLE or MODE_LANDER */
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
        float lander_gravity;   /* px/s^2 */
        float lander_thrust;    /* px/s^2 */
        float lander_turn;      /* d-pad rotation, deg/s */
        float lander_drag;      /* 1/s */
        float lander_ground;    /* friction sliding on the floor, px/s^2 */
} tuning_t;

#define MODE_MARBLE     0
#define MODE_LANDER     1

extern tuning_t tune;

/* The current mode, as an int */
static inline int tuning_mode(void)
{
        return tune.mode >= 0.5f ? MODE_LANDER : MODE_MARBLE;
}

void    tuning_init(PlaydateAPI *pd);
/* Handle input; returns 1 while the panel is open (it eats the d-pad) */
int     tuning_update(PDButtons cur, PDButtons pushed, float dt);
void    tuning_draw(void);
void    tuning_save(void);

#endif
