#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tuning.h"

#define TUNING_FILE     "tuning.txt"

/* Defaults: tuned to be fun rather than usable.  Lots of momentum, very
 * little friction, and the walls mostly let it slide.
 */
static const tuning_t defaults = {
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
};

tuning_t tune;

typedef struct {
        const char *key;        /* in the save file */
        const char *label;
        size_t off;
        float min, max, step;
        int mul;                /* step multiplicatively (for big ranges) */
        const char *fmt;
} param_t;

#define P(f) offsetof(tuning_t, f)
static const param_t params[] = {
        { "tilt_gain",     "Tilt gain",      P(tilt_gain),     100,  20000, 1.15f, 1, "%.0f" },
        { "drag",          "Drag",           P(drag),          0,    8,     0.05f, 0, "%.2f" },
        { "roll_friction", "Rolling friction", P(roll_friction), 0,  600,   4,     0, "%.0f" },
        { "wall_bounce",   "Wall bounce",    P(wall_bounce),   0,    1.5f,  0.05f, 0, "%.2f" },
        { "max_speed",     "Max speed",      P(max_speed),     50,   10000, 1.15f, 1, "%.0f" },
        { "dead_zone",     "Dead zone (g)",  P(dead_zone),     0,    0.2f,  0.002f, 0, "%.3f" },
        { "turn_rate",     "Arrow turn rate", P(turn_rate),    0.5f, 60,    0.5f,  0, "%.1f" },
        { "cam_follow",    "Camera follow",  P(cam_follow),    0.5f, 40,    0.5f,  0, "%.1f" },
        { "cam_lookahead", "Camera lead (s)", P(cam_lookahead), 0,   1,     0.02f, 0, "%.2f" },
        { "sfx_volume",    "Wall sound volume", P(sfx_volume), 0,  1,     0.05f, 0, "%.2f" },
        { "dust",          "Wall dust",      P(dust),          0,    3,     0.1f,  0, "%.1f" },
        { "click_volume",  "Click sound volume", P(click_volume), 0, 1,  0.05f, 0, "%.2f" },
        { "rope_k",        "Bungee stiffness", P(rope_k),      5,    600,   1.15f, 1, "%.0f" },
        { "rope_damp",     "Bungee damping", P(rope_damp),     0,    40,    0.5f,  0, "%.1f" },
        { "swing_damp",    "Bungee swing damping", P(swing_damp), 0, 30,  0.5f,  0, "%.1f" },
};
#define NPARAMS ((int)(sizeof params / sizeof *params))
#define ROW_RESET NPARAMS       /* last row: restore defaults */

static PlaydateAPI *pd;
static LCDFont *font;
static int open, sel, dirty;
static float repeat_t;

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

        if (pushed & kButtonUp)
                sel = (sel + ROW_RESET) % (ROW_RESET + 1);
        if (pushed & kButtonDown)
                sel = (sel + 1) % (ROW_RESET + 1);

        if (sel == ROW_RESET) {
                if (pushed & (kButtonLeft | kButtonRight)) {
                        tune = defaults;
                        dirty = 1;
                }
                return 1;
        }

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

        const int row_h = 15, w = 220, x = LCD_COLUMNS - w - 4;
        /* If the rows don't all fit, show a window that follows sel */
        static int top;
        int vis = (LCD_ROWS - 8 - row_h) / row_h;
        if (vis > ROW_RESET + 1)
                vis = ROW_RESET + 1;
        if (sel < top)
                top = sel;
        if (sel >= top + vis)
                top = sel - vis + 1;
        const int h = vis * row_h + 8 + row_h;
        const int y = (LCD_ROWS - h) / 2;
        if (font)
                pd->graphics->setFont(font);

        pd->graphics->fillRect(x, y, w, h, kColorWhite);
        pd->graphics->drawRect(x, y, w, h, kColorBlack);
        pd->graphics->drawText("Marble physics", 14, kASCIIEncoding, x + 6, y + 3);
        pd->graphics->drawLine(x, y + row_h + 3, x + w - 1, y + row_h + 3, 1, kColorBlack);

        for (int i = top; i < top + vis; i++) {
                int ry = y + row_h + 6 + (i - top) * row_h;
                char val[24];
                const char *label;
                if (i == ROW_RESET) {
                        label = "Reset to defaults";
                        val[0] = 0;
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
