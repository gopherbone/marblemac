#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tuning.h"

#define TUNING_FILE     "tuning.txt"

/* Defaults: tuned to be fun rather than usable.  Lots of momentum, very
 * little friction, and the walls mostly let it slide.  The lander is
 * floaty, a little overpowered and quick to spin: controllable, just.
 */
static const tuning_t defaults = {
        .mode           = MODE_MARBLE,
        .tilt_gain      = 2600.0f,
        .drag           = 0.35f,
        .roll_friction  = 12.0f,
        .wall_bounce    = 0.15f,
        .max_speed      = 2400.0f,
        .dead_zone      = 0.004f,
        .turn_rate      = 14.0f,
        .cam_follow     = 6.0f,
        .cam_lookahead  = 0.12f,
        .sfx_volume     = 0.7f,
        .dust           = 1.0f,
        .click_volume   = 0.7f,
        .rope_k         = 60.0f,
        .rope_damp      = 3.0f,
        .swing_damp     = 5.0f,
        .lander_gravity = 160.0f,
        .lander_thrust  = 420.0f,
        .lander_turn    = 270.0f,
        .lander_drag    = 0.15f,
        .lander_ground  = 300.0f,
        .snag_time      = 0.3f,
};

tuning_t tune;

typedef struct {
        const char *key;        /* in the save file */
        const char *label;
        size_t off;
        float min, max, step;
        int mul;                /* step multiplicatively (for big ranges) */
        const char *fmt;
        int modes;              /* shown in: bit 0 marble, bit 1 lander */
} param_t;

#define M 1
#define L 2
#define ML 3

#define P(f) offsetof(tuning_t, f)
static const param_t params[] = {
        { "mode",          "Mode",           P(mode),          0,    1,     1,     0, "",     ML },
        { "lander_gravity", "Gravity",       P(lander_gravity), 10,  3000,  1.1f,  1, "%.0f", L },
        { "lander_thrust", "Thrust",         P(lander_thrust), 10,   8000,  1.1f,  1, "%.0f", L },
        { "lander_turn",   "Rotation (deg/s)", P(lander_turn), 30,   1080,  15,    0, "%.0f", L },
        { "lander_drag",   "Drag",           P(lander_drag),   0,    5,     0.05f, 0, "%.2f", L },
        { "lander_ground", "Ground friction", P(lander_ground), 0,   3000,  25,    0, "%.0f", L },
        { "tilt_gain",     "Tilt gain",      P(tilt_gain),     100,  20000, 1.15f, 1, "%.0f", M },
        { "drag",          "Drag",           P(drag),          0,    8,     0.05f, 0, "%.2f", M },
        { "roll_friction", "Rolling friction", P(roll_friction), 0,  600,   4,     0, "%.0f", M },
        { "wall_bounce",   "Wall bounce",    P(wall_bounce),   0,    1.5f,  0.05f, 0, "%.2f", ML },
        { "max_speed",     "Max speed",      P(max_speed),     50,   10000, 1.15f, 1, "%.0f", ML },
        { "dead_zone",     "Dead zone (g)",  P(dead_zone),     0,    0.2f,  0.002f, 0, "%.3f", M },
        { "turn_rate",     "Arrow turn rate", P(turn_rate),    0.5f, 60,    0.5f,  0, "%.1f", M },
        { "cam_follow",    "Camera follow",  P(cam_follow),    0.5f, 40,    0.5f,  0, "%.1f", ML },
        { "cam_lookahead", "Camera lead (s)", P(cam_lookahead), 0,   1,     0.02f, 0, "%.2f", ML },
        { "sfx_volume",    "Wall/engine volume", P(sfx_volume), 0, 1,     0.05f, 0, "%.2f", ML },
        { "dust",          "Wall dust",      P(dust),          0,    3,     0.1f,  0, "%.1f", ML },
        { "click_volume",  "Click sound volume", P(click_volume), 0, 1,  0.05f, 0, "%.2f", ML },
        { "snag_time",     "Click snag (s)", P(snag_time),     0,    1.5f,  0.05f, 0, "%.2f", ML },
        { "rope_k",        "Bungee stiffness", P(rope_k),      5,    600,   1.15f, 1, "%.0f", ML },
        { "rope_damp",     "Bungee damping", P(rope_damp),     0,    40,    0.5f,  0, "%.1f", ML },
        { "swing_damp",    "Bungee swing damping", P(swing_damp), 0, 30,  0.5f,  0, "%.1f", M },
};
#define NPARAMS ((int)(sizeof params / sizeof *params))
#define ROW_MODE 0              /* first row: marble or lander */
#define ROW_RESET NPARAMS       /* last row: restore defaults */
#define NROWS (ROW_RESET + 1)

static PlaydateAPI *pd;
static LCDFont *font;
static int open, sel, dirty;
static float repeat_t, mode_crank;

/* Is row i shown in the current mode? */
static int visible(int i)
{
        return i == ROW_RESET || params[i].modes & (1 << tuning_mode());
}

/* The next shown row from `from` in direction dir (wrapping) */
static int step_row(int from, int dir)
{
        int i = from;
        do
                i = (i + dir + NROWS) % NROWS;
        while (!visible(i) && i != from);
        return i;
}

static void set_mode(int m)
{
        tune.mode = m;
        dirty = 1;
}

static float *field(const param_t *p)
{
        return (float *)((char *)&tune + p->off);
}

static void nudge(int dir, float amount)
{
        const param_t *p = &params[sel];
        float *v = field(p);
        if (p->mul)
                *v *= powf(p->step, dir * amount);
        else
                *v += p->step * dir * amount;
        *v = fmaxf(p->min, fminf(p->max, *v));
        dirty = 1;
}

static void load(void)
{
        SDFile *f = pd->file->open(TUNING_FILE, kFileReadData);
        if (!f)
                return;
        char buf[1024];
        int n = pd->file->read(f, buf, sizeof buf - 1);
        pd->file->close(f);
        if (n <= 0)
                return;
        buf[n] = 0;
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
                char *sp = strchr(line, ' ');
                if (!sp)
                        continue;
                *sp = 0;
                for (int i = 0; i < NPARAMS; i++) {
                        if (!strcmp(line, params[i].key)) {
                                float v = strtof(sp + 1, NULL);
                                *field(&params[i]) = fmaxf(params[i].min, fminf(params[i].max, v));
                        }
                }
        }
        tune.mode = tuning_mode();
}

void tuning_save(void)
{
        if (!dirty)
                return;
        SDFile *f = pd->file->open(TUNING_FILE, kFileWrite);
        if (!f)
                return;
        for (int i = 0; i < NPARAMS; i++) {
                char line[64];
                int n = snprintf(line, sizeof line, "%s %g\n", params[i].key, (double)*field(&params[i]));
                pd->file->write(f, line, n);
        }
        pd->file->close(f);
        dirty = 0;
}

void tuning_init(PlaydateAPI *playdate)
{
        pd = playdate;
        tune = defaults;
        load();
        const char *err;
        font = pd->graphics->loadFont("/System/Fonts/Roobert-10-Bold.pft", &err);
}

int tuning_update(PDButtons cur, PDButtons pushed, float dt)
{
        if (pushed & kButtonB) {
                open = !open;
                if (!open)
                        tuning_save();
        }
        if (!open)
                return 0;

        if (!visible(sel))
                sel = ROW_MODE;
        if (pushed & kButtonUp)
                sel = step_row(sel, -1);
        if (pushed & kButtonDown)
                sel = step_row(sel, 1);

        if (sel == ROW_RESET) {
                if (pushed & (kButtonLeft | kButtonRight)) {
                        /* (everything but the mode) */
                        float mode = tune.mode;
                        tune = defaults;
                        tune.mode = mode;
                        dirty = 1;
                }
                pd->system->getCrankChange();
                return 1;
        }

        if (sel == ROW_MODE) {
                /* Left/right flip it, as does a good turn of the crank */
                if (pushed & (kButtonLeft | kButtonRight))
                        set_mode(!tuning_mode());
                mode_crank += pd->system->getCrankChange();
                if (fabsf(mode_crank) >= 45) {
                        set_mode(!tuning_mode());
                        mode_crank = 0;
                }
                return 1;
        }
        mode_crank = 0;

        int dir = (cur & kButtonRight) ? 1 : (cur & kButtonLeft) ? -1 : 0;
        if (pushed & (kButtonLeft | kButtonRight)) {
                nudge(dir, 1);
                repeat_t = -0.35f;      /* delay before auto-repeat */
        } else if (dir) {
                repeat_t += dt;
                while (repeat_t > 0.05f) {
                        nudge(dir, 1);
                        repeat_t -= 0.05f;
                }
        }

        /* Crank: one step per 15 degrees, fractional in between */
        float crank = pd->system->getCrankChange();
        if (crank != 0)
                nudge(crank > 0 ? 1 : -1, fabsf(crank) / 15.0f);
        return 1;
}

void tuning_draw(void)
{
        if (!open)
                return;

        /* The rows shown in this mode, and where sel is among them */
        int rows[NROWS], nrows = 0, cur = 0;
        for (int i = 0; i < NROWS; i++) {
                if (!visible(i))
                        continue;
                if (i == sel)
                        cur = nrows;
                rows[nrows++] = i;
        }

        const int row_h = 15, w = 220, x = LCD_COLUMNS - w - 4;
        /* If the rows don't all fit, show a window that follows sel */
        static int top;
        int vis = (LCD_ROWS - 8 - row_h) / row_h;
        if (vis > nrows)
                vis = nrows;
        if (top > nrows - vis)
                top = nrows - vis;
        if (cur < top)
                top = cur;
        if (cur >= top + vis)
                top = cur - vis + 1;
        const int h = vis * row_h + 8 + row_h;
        const int y = (LCD_ROWS - h) / 2;
        if (font)
                pd->graphics->setFont(font);

        pd->graphics->fillRect(x, y, w, h, kColorWhite);
        pd->graphics->drawRect(x, y, w, h, kColorBlack);
        const char *title = tuning_mode() == MODE_LANDER ? "Lander physics" : "Marble physics";
        pd->graphics->drawText(title, strlen(title), kASCIIEncoding, x + 6, y + 3);
        pd->graphics->drawLine(x, y + row_h + 3, x + w - 1, y + row_h + 3, 1, kColorBlack);

        for (int r = top; r < top + vis; r++) {
                int i = rows[r];
                int ry = y + row_h + 6 + (r - top) * row_h;
                char val[24];
                const char *label;
                if (i == ROW_RESET) {
                        label = "Reset to defaults";
                        val[0] = 0;
                } else if (i == ROW_MODE) {
                        label = params[i].label;
                        snprintf(val, sizeof val, "%s", tuning_mode() == MODE_LANDER ? "Lander" : "Marble");
                } else {
                        label = params[i].label;
                        snprintf(val, sizeof val, params[i].fmt, (double)*field(&params[i]));
                }
                if (i == sel) {
                        pd->graphics->fillRect(x + 2, ry - 1, w - 4, row_h, kColorBlack);
                        pd->graphics->setDrawMode(kDrawModeFillWhite);
                }
                pd->graphics->drawText(label, strlen(label), kASCIIEncoding, x + 6, ry);
                int vw = pd->graphics->getTextWidth(font, val, strlen(val), kASCIIEncoding, 0);
                pd->graphics->drawText(val, strlen(val), kASCIIEncoding, x + w - 8 - vw, ry);
                pd->graphics->setDrawMode(kDrawModeCopy);
        }
}
