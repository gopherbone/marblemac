/* Marble Mac: a Mac Plus on the Playdate, driven by tilting.
 *
 * The cursor is a marble rolling on the tilted board that is the Mac's
 * screen.  The Playdate shows a 400x240 window onto the 512x342 Mac
 * display, at 1:1, following the marble.  The arrow points the way it's
 * rolling.  This is deliberately not a good way to use a computer.
 *
 * Controls: tilt to roll, A = mouse button, Down = bungee (crank reels).
 *
 * Or, picked in the B panel, the cursor is a lunar lander instead:
 * gravity pulls it down the screen, Up fires the engine along the nose,
 * the crank points the nose (0 = up; docked, Left/Right turn it), A is
 * still the button, and the bungee is Down (tap: drop/let go; held: Up
 * reels in, Right pays out).
 *
 * Emulation is uMac (Matt Evans) on Musashi.  Needs a Mac Plus v3 ROM
 * ("rom.bin") and a raw disk image ("disk.img"), either in the game's
 * Data folder or bundled in Source/.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "pd_api.h"

#include "umac.h"
#include "machw.h"
#include "disc.h"
#include "rom.h"

#include "cursor.h"
#include "macglue.h"
#include "vdisk.h"
#include "tuning.h"
#include "sfx.h"
#include "dust.h"
#include "wallfx.h"
#include "clickfx.h"
#include "bungee.h"
#include "lander.h"
#ifdef UMAC_JIT
#include "jit_playdate.h"
#include "m68kjit.h"
#endif

static PlaydateAPI *pd;
PlaydateAPI *marble_pd;     /* for device_stubs.c */

#define MAC_W           DISP_WIDTH
#define MAC_H           DISP_HEIGHT
#define MAC_STRIDE      (MAC_W / 8)

#define ROM_FILE        "rom.bin"
#define DISK_FILE       "disk.img"
#define OVERLAY_FILE    "disk.ovl"      /* the Mac's writes to disk.img */

/* Emulation timing: umac_loop() runs one 5ms quantum of 68K time. */
#define QUANTUM_US      5000
#define VSYNC_US        16626   /* 60.15Hz */
#define EMU_BUDGET_MS   28      /* wall time we'll spend emulating per frame */
#define MAX_LAG_US      100000

/* Marble physics parameters live in tuning.c (B opens a panel) */

/* Camera follows the marble, slightly lagging and leading. */
#define CAM_MARGIN      2.0f    /* px between cursor and the edge of the view */

static uint8_t *mac_ram;
static uint8_t *mac_rom;
static SDFile *disk_file;
static vdisk_t vdisk;
static int have_disk;
static const char *fatal_msg;
static LCDFont *ui_font;

static uint64_t emu_us;         /* emulated time */
static uint64_t target_us;      /* wall time the emulator should reach */
static uint64_t next_vsync_us;
static uint64_t next_1hz_us;
static unsigned int last_ms;

static float ax_f, ay_f, az_f;  /* filtered accelerometer */
static float ref[3] = { 0, 0, 1 };
static int need_calibrate = 15; /* frames until we sample the "level" pose */

static int mac_ready;           /* booted far enough to drive the cursor */
static float mx, my, vx, vy;    /* marble */
static float angle;             /* direction arrow points (the lander's nose) */
static int cur_mode = -1;       /* MODE_MARBLE / MODE_LANDER being simulated */
static float cam_x, cam_y;
static float ext_x0, ext_x1, ext_y0, ext_y1;   /* cursor footprint around hotspot */

static int show_stats;
static float stat_speed, stat_fps;
static uint64_t stat_emu0;
static unsigned int stat_ms0, stat_frames;
#ifdef MARBLE_PROFILE
static unsigned int prof_ms0;
#endif

////////////////////////////////////////////////////////////////////////////////
// Files

static uint8_t *load_file(const char *name, unsigned int *size_out, int *from_data)
{
        SDFile *f = pd->file->open(name, kFileReadData);
        *from_data = (f != NULL);
        if (!f)
                f = pd->file->open(name, kFileRead);
        if (!f)
                return NULL;

        pd->file->seek(f, 0, SEEK_END);
        int size = pd->file->tell(f);
        pd->file->seek(f, 0, SEEK_SET);
        if (size <= 0) {
                pd->file->close(f);
                return NULL;
        }
        uint8_t *buf = pd->system->realloc(NULL, size);
        if (!buf) {
                pd->file->close(f);
                return NULL;
        }
        int got = 0;
        while (got < size) {
                int r = pd->file->read(f, buf + got, size - got);
                if (r <= 0)
                        break;
                got += r;
        }
        pd->file->close(f);
        if (got != size) {
                pd->system->realloc(buf, 0);
                return NULL;
        }
        *size_out = size;
        return buf;
}

/* The disk image is read straight from its file as the Mac asks for
 * sectors; the Mac's writes go to a vdisk overlay that we save to the
 * Data folder when the console locks or the game quits.
 */
/* Disk read stats, for the 5s log line */
static unsigned disk_reads, disk_read_kb;
static float disk_read_s;
static int disk_base_read_file(uint8_t *buf, uint32_t offset, uint32_t len);

static int disk_base_read(void *ctx, uint8_t *buf, uint32_t offset, uint32_t len)
{
        (void)ctx;
        float t0 = pd->system->getElapsedTime();
        disk_reads++;
        disk_read_kb += len / 1024;
        int r = disk_base_read_file(buf, offset, len);
        disk_read_s += pd->system->getElapsedTime() - t0;
        return r;
}

static int disk_base_read_file(uint8_t *buf, uint32_t offset, uint32_t len)
{
        if (pd->file->seek(disk_file, offset, SEEK_SET) < 0)
                return -1;
        uint32_t got = 0;
        while (got < len) {
                int r = pd->file->read(disk_file, buf + got, len - got);
                if (r <= 0)
                        return -1;
                got += r;
        }
        return 0;
}

static int overlay_write(void *ctx, const void *buf, uint32_t len)
{
        return pd->file->write((SDFile *)ctx, buf, len) == (int)len ? 0 : -1;
}

static void *pd_realloc(void *p, size_t size)
{
        return pd->system->realloc(p, size);
}

static int open_disk(void)
{
        disk_file = pd->file->open(DISK_FILE, kFileReadData);
        if (!disk_file)
                disk_file = pd->file->open(DISK_FILE, kFileRead);
        if (!disk_file)
                return 0;
        pd->file->seek(disk_file, 0, SEEK_END);
        int size = pd->file->tell(disk_file);
        if (size <= 0 || vdisk_init(&vdisk, size, disk_base_read, NULL, pd_realloc) < 0) {
                pd->file->close(disk_file);
                return 0;
        }

        unsigned int olen;
        int dummy;
#ifdef MARBLE_TIMING
        uint8_t *ovl = NULL;            /* always the same fresh disk */
        (void)olen; (void)dummy;
#else
        uint8_t *ovl = load_file(OVERLAY_FILE, &olen, &dummy);
#endif
        if (ovl) {
                if (vdisk_load_overlay(&vdisk, ovl, olen) < 0)
                        pd->system->logToConsole("Ignoring " OVERLAY_FILE " (doesn't match " DISK_FILE ")");
                pd->system->realloc(ovl, 0);
        }
        return 1;
}

static void save_disk(void)
{
#ifdef MARBLE_TIMING
        return;
#endif
        if (!have_disk || !vdisk.dirty)
                return;

        SDFile *f = pd->file->open(OVERLAY_FILE ".tmp", kFileWrite);
        if (!f) {
                pd->system->logToConsole("Can't write " OVERLAY_FILE ".tmp");
                return;
        }
        int r = vdisk_save_overlay(&vdisk, overlay_write, f);
        pd->file->close(f);
        if (r < 0) {
                pd->file->unlink(OVERLAY_FILE ".tmp", 0);
                pd->system->logToConsole("Short write saving " OVERLAY_FILE);
                vdisk.dirty = 1;
                return;
        }
        /* Keep the previous save: the Mac's file system is live when we
         * snapshot it (catalog changes can still be sitting in its caches),
         * so a save can occasionally catch a file mid-update.
         */
        pd->file->unlink(OVERLAY_FILE ".prev", 0);
        pd->file->rename(OVERLAY_FILE, OVERLAY_FILE ".prev");
        pd->file->rename(OVERLAY_FILE ".tmp", OVERLAY_FILE);
}

////////////////////////////////////////////////////////////////////////////////
// Tilt

static void read_accel(void)
{
        float x, y, z;
        pd->system->getAccelerometer(&x, &y, &z);
        const float k = 0.3f;
        ax_f += (x - ax_f) * k;
        ay_f += (y - ay_f) * k;
        az_f += (z - az_f) * k;
}

static void calibrate(void)
{
        float n = sqrtf(ax_f * ax_f + ay_f * ay_f + az_f * az_f);
        if (n < 0.3f) {
                need_calibrate = 5;     /* Sensor not awake yet, try again */
                return;
        }
        ref[0] = ax_f / n;
        ref[1] = ay_f / n;
        ref[2] = az_f / n;
        need_calibrate = 0;
}

/* Rotate the current gravity reading so that the calibrated pose reads as
 * "flat on its back" (0,0,1); x/y of the result are then the board tilt.
 * (Rodrigues' rotation taking ref onto +z.)
 */
static void get_tilt(float *tx, float *ty)
{
        float g[3] = { ax_f, ay_f, az_f };
        float c = ref[2];
        if (c < -0.999f) {
                /* Calibrated upside down: flip about x */
                *tx = g[0];
                *ty = -g[1];
                return;
        }
        /* v = ref x z = (ref_y, -ref_x, 0) */
        float v0 = ref[1], v1 = -ref[0];
        float k = 1.0f / (1.0f + c);
        /* R = I + [v]x + [v]x^2 * k, rows 0 and 1 only */
        float r00 = 1 - k * v1 * v1;
        float r01 = k * v0 * v1;
        float r02 = v1;
        float r10 = k * v0 * v1;
        float r11 = 1 - k * v0 * v0;
        float r12 = -v0;
        *tx = r00 * g[0] + r01 * g[1] + r02 * g[2];
        *ty = r10 * g[0] + r11 * g[1] + r12 * g[2];
}

static float noise_floor(float t)
{
        if (t > tune.dead_zone)
                return t - tune.dead_zone;
        if (t < -tune.dead_zone)
                return t + tune.dead_zone;
        return 0;
}

static float cursor_rotation(const mac_cursor_t *c)
{
        return cursor_is_pointer(c) ? angle - CURSOR_ARROW_ANGLE : 0;
}

static void update_extent(void)
{
        mac_cursor_t c;
        mac_get_cursor(&c);
        cursor_extent(&c, cursor_rotation(&c), &ext_x0, &ext_x1, &ext_y0, &ext_y1);
}

static void step_marble(float dt)
{
        float tx, ty;
        get_tilt(&tx, &ty);
        vx += noise_floor(tx) * tune.tilt_gain * dt;
        vy += noise_floor(ty) * tune.tilt_gain * dt;
        bungee_apply(mx, my, &vx, &vy, noise_floor(tx) * tune.tilt_gain,
                     noise_floor(ty) * tune.tilt_gain, dt);

        float drag = expf(-tune.drag * dt);
        vx *= drag;
        vy *= drag;

        float sp = sqrtf(vx * vx + vy * vy);
        if (sp > 0) {
                float ns = sp - tune.roll_friction * dt;
                if (ns > tune.max_speed)
                        ns = tune.max_speed;
                if (ns < 0)
                        ns = 0;
                vx *= ns / sp;
                vy *= ns / sp;
        }

        mx += vx * dt;
        my += vy * dt;

        /* Point the arrow where we're rolling */
        sp = sqrtf(vx * vx + vy * vy);
        if (sp > 20.0f) {
                float target = atan2f(vy, vx);
                float diff = remainderf(target - angle, 2 * (float)M_PI);
                float k = fminf(1.0f, dt * tune.turn_rate);
                angle = remainderf(angle + diff * k, 2 * (float)M_PI);
        }
        /* ...unless it's hanging from the bungee: then it swings */
        angle = bungee_arrow(angle, vx, vy, noise_floor(tx) * tune.tilt_gain,
                             noise_floor(ty) * tune.tilt_gain, dt);

        /* The whole drawn cursor, not just its hotspot, stays on the Mac
         * screen.  Hitting an edge kills most of the speed into it, so the
         * marble slides along the wall instead of bouncing off.
         */
        update_extent();
        float lo_x = -ext_x0, hi_x = MAC_W - ext_x1;
        float lo_y = -ext_y0, hi_y = MAC_H - ext_y1;
        float in_vx = vx, in_vy = vy;   /* for wallfx: speed before the bounce */
        if (mx < lo_x) {
                mx = lo_x;
                if (vx < 0)
                        vx = -vx * tune.wall_bounce;
        } else if (mx > hi_x) {
                mx = hi_x;
                if (vx > 0)
                        vx = -vx * tune.wall_bounce;
        }
        if (my < lo_y) {
                my = lo_y;
                if (vy < 0)
                        vy = -vy * tune.wall_bounce;
        } else if (my > hi_y) {
                my = hi_y;
                if (vy > 0)
                        vy = -vy * tune.wall_bounce;
        }

        /* Thunks, scraping and dust */
        wallfx_step(&(wallfx_marble_t){
                .x = mx, .y = my, .vx = in_vx, .vy = in_vy, .ovx = vx, .ovy = vy,
                .x0 = ext_x0, .x1 = ext_x1, .y0 = ext_y0, .y1 = ext_y1,
                .gx = noise_floor(tx) * tune.tilt_gain, .gy = noise_floor(ty) * tune.tilt_gain,
        }, dt, MAC_W, MAC_H);
}

/* The lander: gravity, the engine, the nose steered by the crank or the
 * d-pad.  Same walls, sounds and dust as the marble.
 */
static void step_lander(float dt, PDButtons cur, int panel_open)
{
        const lander_params_t p = {
                .gravity = tune.lander_gravity,
                .thrust = tune.lander_thrust,
                .turn = tune.lander_turn,
                .drag = tune.lander_drag,
                .ground_friction = tune.lander_ground,
                .max_speed = tune.max_speed,
                .wall_bounce = tune.wall_bounce,
        };
        /* While Down is held the d-pad works the bungee's reel */
        int roping = (cur & kButtonDown) != 0;
        int controls = !panel_open && !roping;
        lander_steer_t st = {
                .steer = !panel_open,
                .docked = pd->system->isCrankDocked(),
                .crank_deg = pd->system->getCrankAngle(),
                .rot = controls ? ((cur & kButtonRight) != 0) - ((cur & kButtonLeft) != 0) : 0,
        };
        angle = lander_steer(angle, &st, &p, dt);
        int thrust = controls && (cur & kButtonUp);

        float ax, ay;
        lander_accel(angle, thrust, &p, &ax, &ay);
        vx += ax * dt;
        vy += ay * dt;
        /* (the nose wins over the rope: no pendulum here) */
        bungee_apply(mx, my, &vx, &vy, 0, p.gravity, dt);

        update_extent();
        float in_vx, in_vy;
        lander_move(&mx, &my, &vx, &vy, &p, ext_x0, ext_x1, ext_y0, ext_y1,
                    MAC_W, MAC_H, dt, &in_vx, &in_vy);

        wallfx_step(&(wallfx_marble_t){
                .x = mx, .y = my, .vx = in_vx, .vy = in_vy, .ovx = vx, .ovy = vy,
                .x0 = ext_x0, .x1 = ext_x1, .y0 = ext_y0, .y1 = ext_y1,
                .gx = 0, .gy = p.gravity, .lander = 1,
        }, dt, MAC_W, MAC_H);
        lander_flame_step(thrust, mx, my, angle, MAC_W, MAC_H, tune.dust, dt);
        sfx_thrust(thrust ? 1.0f : 0.0f);
}

/* Switching modes: stop dead where it is (pointing wherever it was), so
 * neither mode inherits the other's momentum */
static void set_mode(int mode)
{
        cur_mode = mode;
        vx = vy = 0;
        bungee_swing_reset();
}

static void step_camera(float dt)
{
        float tx = mx + vx * tune.cam_lookahead - LCD_COLUMNS / 2;
        float ty = my + vy * tune.cam_lookahead - LCD_ROWS / 2;
        tx = fmaxf(0, fminf(tx, MAC_W - LCD_COLUMNS));
        ty = fmaxf(0, fminf(ty, MAC_H - LCD_ROWS));
        float k = fminf(1.0f, dt * tune.cam_follow);
        cam_x += (tx - cam_x) * k;
        cam_y += (ty - cam_y) * k;

        /* However far behind the camera is, the cursor shoves it along so
         * it's always entirely in view.
         */
        const float m = CAM_MARGIN;
        cam_x = fminf(cam_x, mx + ext_x0 - m);
        cam_x = fmaxf(cam_x, mx + ext_x1 + m - LCD_COLUMNS);
        cam_y = fminf(cam_y, my + ext_y0 - m);
        cam_y = fmaxf(cam_y, my + ext_y1 + m - LCD_ROWS);
        cam_x = fmaxf(0, fminf(cam_x, MAC_W - LCD_COLUMNS));
        cam_y = fmaxf(0, fminf(cam_y, MAC_H - LCD_ROWS));
}

////////////////////////////////////////////////////////////////////////////////
// Emulation

#ifdef MARBLE_BENCH
void marble_bench(PlaydateAPI *pd, void *heap_block);
static float bench_q_time;
static int bench_q_n;
#endif

static void emu_quantum(void)
{
#ifdef MARBLE_BENCH
        float t0 = pd->system->getElapsedTime();
        umac_loop();
        bench_q_time += pd->system->getElapsedTime() - t0;
        if (++bench_q_n == 400) {
                extern unsigned long m68k_instruction_count;
                pd->system->logToConsole("bench: umac_loop (5ms of 68k) takes %.2f ms, %lu instr/quantum, %.0f ns/instr",
                                         (double)(bench_q_time * 1000 / bench_q_n), m68k_instruction_count / bench_q_n,
                                         (double)(bench_q_time * 1e9f / m68k_instruction_count));
                m68k_instruction_count = 0;
                bench_q_time = 0;
                bench_q_n = 0;
        }
#else
        umac_loop();
#endif
        emu_us += QUANTUM_US;
        while (emu_us >= next_vsync_us) {
                umac_vsync_event();
                next_vsync_us += VSYNC_US;
        }
        while (emu_us >= next_1hz_us) {
                umac_1hz_event();
                next_1hz_us += 1000000;
        }
}

static int paced;               /* display refresh capped (the Mac is keeping up) */

static void emu_run(unsigned int wall_dt_ms)
{
        unsigned int start = pd->system->getCurrentTimeMilliseconds();
        int behind = 0;

        target_us += (uint64_t)wall_dt_ms * 1000;
        if (target_us > emu_us + MAX_LAG_US)
                target_us = emu_us + MAX_LAG_US;

        while (emu_us < target_us) {
                emu_quantum();
                if (pd->system->getCurrentTimeMilliseconds() - start >= EMU_BUDGET_MS) {
                        /* Can't keep up: let the Mac run slow instead of spiralling */
                        target_us = emu_us;
                        behind = 1;
                        break;
                }
        }
        /* Keeping up (often idle): no point drawing faster than the
         * display's 50Hz.  Behind: unpaced, so a frame that overruns a
         * 20ms slot doesn't then wait for the next one.
         */
        if (behind == paced) {
                paced = !behind;
                pd->display->setRefreshRate(paced ? 50 : 0);
        }
}

#ifdef MARBLE_TIMING
/* Repeatable device benchmark: boot the same fresh disk, emulating flat
 * out with scripted input (below), and log the device time each 5
 * emulated seconds took.  (Build with -DMARBLE_TIMING.)
 *
 * A fixed bit of work once the Finder is up (the same as jit_verify's
 * test script): open the disk, the Games folder, then Tetris.  {emulated
 * ms, x, y}: double-click there.
 */
static const struct { unsigned ms; int x, y; } timing_script[] = {
        { 22000, 470, 45 }, { 25000, 175, 110 }, { 35000, 182, 188 },
};

static void timing_input(void)
{
        unsigned ms = (unsigned)(emu_us / 1000);
        int x = MAC_W / 2, y = MAC_H / 2, button = 0;
        for (unsigned i = 0; i < sizeof timing_script / sizeof *timing_script; i++) {
                if (ms < timing_script[i].ms)
                        break;
                x = timing_script[i].x;
                y = timing_script[i].y;
                /* there for 500ms first, then down 75, up 75, down 75 */
                unsigned t = ms - timing_script[i].ms;
                button = (t >= 500 && t < 575) || (t >= 650 && t < 725);
        }
        if (mac_ready)
                mac_set_mouse(x, y);
        umac_mouse(0, 0, button);
}

/* Checkpoint screenshots (raw Mac framebuffer, tools/pd2png.py makes a
 * PNG) in the Data folder, to see what the script actually did */
static void timing_shots(void)
{
        static const unsigned at[] = { 21, 24, 27, 30, 37, 45 };
        static unsigned next;
        if (next >= sizeof at / sizeof *at || emu_us < (uint64_t)at[next] * 1000000)
                return;
        char name[32];
        snprintf(name, sizeof name, "shot_%02u.bin", at[next]);
        SDFile *f = pd->file->open(name, kFileWrite);
        if (f) {
                const uint8_t *fb = mac_ram + umac_get_fb_offset();
                static uint8_t inv[MAC_STRIDE * MAC_H];
                for (unsigned i = 0; i < sizeof inv; i++)
                        inv[i] = ~fb[i];
                pd->file->write(f, inv, sizeof inv);
                pd->file->close(f);
        }
        next++;
}

static void timing_run(void)
{
        static float spent;
        static uint64_t next_mark = 5000000;
        unsigned int start = pd->system->getCurrentTimeMilliseconds();
        while (pd->system->getCurrentTimeMilliseconds() - start < EMU_BUDGET_MS) {
                timing_input();
                timing_shots();
                float t0 = pd->system->getElapsedTime();
                emu_quantum();
                spent += pd->system->getElapsedTime() - t0;
                if (emu_us >= next_mark) {
                        pd->system->logToConsole("timing: emulated %2us-%2us took %.0f ms (disk: %u reads, %u KB, %.0f ms)",
                                                 (unsigned)(next_mark / 1000000 - 5), (unsigned)(next_mark / 1000000),
                                                 (double)(spent * 1000), disk_reads, disk_read_kb, (double)(disk_read_s * 1000));
                        disk_reads = disk_read_kb = 0;
                        disk_read_s = 0;
                        spent = 0;
                        next_mark += 5000000;
                }
        }
        target_us = emu_us;
}
#endif

static void mac_reset(void *ud)
{
        (void)ud;
        umac_reset();
        mac_ready = 0;
}

////////////////////////////////////////////////////////////////////////////////
// Display

/* The frame is drawn here first, then only rows that differ from what's
 * on the display get copied and marked: the LCD transfer is the slow
 * part, and while the Mac sits still most rows don't change.
 */
static uint8_t back[LCD_ROWS * LCD_ROWSIZE] __attribute__((aligned(4)));

static void present(void)
{
        uint8_t *frame = pd->graphics->getFrame();
        int run = -1;
        for (int y = 0; y < LCD_ROWS; y++) {
                uint8_t *dst = frame + y * LCD_ROWSIZE;
                const uint8_t *src = back + y * LCD_ROWSIZE;
                int changed = memcmp(dst, src, LCD_COLUMNS / 8) != 0;
                if (changed) {
                        memcpy(dst, src, LCD_COLUMNS / 8);
                        if (run < 0)
                                run = y;
                } else if (run >= 0) {
                        pd->graphics->markUpdatedRows(run, y - 1);
                        run = -1;
                }
        }
        if (run >= 0)
                pd->graphics->markUpdatedRows(run, LCD_ROWS - 1);
}

static void blit(void)
{
        uint8_t *frame = back;
        const uint8_t *fb = mac_ram + umac_get_fb_offset();
        /* Only scroll in 2px steps: the Mac's dither patterns (the 50%
         * grey desktop especially) repeat every 2px, so odd moves make
         * the whole screen flip phase and shimmer.
         */
        int cx = (int)(cam_x / 2 + 0.5f) * 2;
        int cy = (int)(cam_y / 2 + 0.5f) * 2;
        int b = cx >> 3, s = cx & 7;

        /* We draw the cursor ourselves, so lift the Mac's own one off the
         * framebuffer while we copy it.
         */
        mac_crsr_undo_t undo;
        if (mac_ready)
                mac_erase_cursor(&undo);

        /* Mac is 1=black, Playdate is 1=white.  A word at a time, 13 per
         * row (covering the 2 padding bytes); the last reads run a few
         * bytes past the row (or the framebuffer, into the RAM after it),
         * which is harmless.
         */
        for (int y = 0; y < LCD_ROWS; y++) {
                const uint8_t *src = fb + (cy + y) * MAC_STRIDE + b;
                uint8_t *dst = frame + y * LCD_ROWSIZE;
                for (int i = 0; i < LCD_ROWSIZE; i += 4) {
                        uint32_t w;
                        memcpy(&w, src + i, 4);
                        w = __builtin_bswap32(w);
                        if (s)
                                w = w << s | src[i + 4] >> (8 - s);
                        w = ~__builtin_bswap32(w);
                        memcpy(dst + i, &w, 4);
                }
        }

        /* Wall dust, under the cursor, locked to the same snapped camera */
        dust_draw(frame, LCD_ROWSIZE, LCD_COLUMNS, LCD_ROWS, cx, cy);
        bungee_draw(frame, LCD_ROWSIZE, LCD_COLUMNS, LCD_ROWS, cx, cy, (int)mx, (int)my);

        if (mac_ready) {
                mac_unerase_cursor(&undo);
                if (mac_cursor_visible()) {
                        if (cur_mode == MODE_LANDER)
                                lander_flame_draw(frame, LCD_ROWSIZE, LCD_COLUMNS, LCD_ROWS,
                                                  (int)mx - cx, (int)my - cy, angle);
                        mac_cursor_t c;
                        mac_get_cursor(&c);
                        cursor_draw(frame, LCD_ROWSIZE, LCD_COLUMNS, LCD_ROWS, &c,
                                    (int)mx - cx, (int)my - cy, cursor_rotation(&c));
                }
        }
        present();
}

static void draw_stats(unsigned int now)
{
        stat_frames++;
        if (now - stat_ms0 >= 1000) {
                float sec = (now - stat_ms0) / 1000.0f;
                stat_speed = (emu_us - stat_emu0) / 10000.0f / sec;
                stat_fps = stat_frames / sec;
                stat_emu0 = emu_us;
                stat_ms0 = now;
                stat_frames = 0;
                /* One short line every 5s: serial output is slow on the device */
                static int every;
                if (++every % 5 == 0) {
                        pd->system->logToConsole("marblemac: 68k %.0f%% %.0ffps booted=%d disk %u reads %u KB %.0f ms",
                                                 (double)stat_speed, (double)stat_fps, mac_ready,
                                                 disk_reads, disk_read_kb, (double)(disk_read_s * 1000));
                        disk_reads = disk_read_kb = 0;
                        disk_read_s = 0;
#if defined(UMAC_JIT) && defined(MARBLE_PROFILE)
                        m68kjit_stats_t *js = &m68k_jit_stats;
                        pd->system->logToConsole("marblemac: natives %lu calls %lu instrs (%.0fms), aline %.0fms, idle quanta %lu",
                                                 (unsigned long)js->native_calls, (unsigned long)js->native_instrs,
                                                 (double)js->t_native * 1000, (double)js->t_aline * 1000,
                                                 (unsigned long)js->idle_quanta);
                        js->t_native = js->t_aline = 0;
                        pd->system->logToConsole("marblemac: %.1fs: emu %.0fms (run %.0f xlat %.0f interp %.0f) jit %lu interp %lu blocks %lu xlat %lu",
                                                 (double)(now - prof_ms0) / 1000, (double)js->t_total * 1000,
                                                 (double)js->t_run * 1000, (double)js->t_xlat * 1000,
                                                 (double)js->t_interp * 1000, (unsigned long)js->jit_instrs,
                                                 (unsigned long)js->interp_instrs, (unsigned long)js->blocks,
                                                 (unsigned long)js->translations);
                        js->t_total = js->t_run = js->t_xlat = js->t_interp = 0;
                        prof_ms0 = now;
#endif
                }
        }
        if (!show_stats)
                return;
        char buf[48];
        int n = snprintf(buf, sizeof buf, "68k %3.0f%%  %2.0ffps", (double)stat_speed, (double)stat_fps);
        if (ui_font)
                pd->graphics->setFont(ui_font);
        pd->graphics->fillRect(0, LCD_ROWS - 18, 150, 18, kColorWhite);
        pd->graphics->drawText(buf, n, kASCIIEncoding, 2, LCD_ROWS - 17);
}

static void draw_fatal(void)
{
        pd->graphics->clear(kColorWhite);
        const char *s = fatal_msg;
        for (int y = 10; *s; y += 20) {         /* one line per '\n' */
                size_t n = strcspn(s, "\n");
                pd->graphics->drawText(s, n, kASCIIEncoding, 10, y);
                s += n + (s[n] == '\n');
        }
}

////////////////////////////////////////////////////////////////////////////////

#ifdef MARBLE_FRAMEPROF
/* Where a frame's time goes: input+physics, emulation, drawing, overlays,
 * and whatever the system does between our update() calls.
 */
static float fp_t[5], fp_last_end;
static int fp_frames;
#define FP_MARK(i) do { float _t = pd->system->getElapsedTime(); fp_t[i] += _t - fp_mark; fp_mark = _t; } while (0)
#else
#define FP_MARK(i) do { } while (0)
#endif

static int update(void *ud)
{
        (void)ud;
#ifdef MARBLE_FRAMEPROF
        float fp_mark = pd->system->getElapsedTime();
        if (fp_last_end > 0)
                fp_t[4] += fp_mark - fp_last_end;
        if (++fp_frames == 100) {
                pd->system->logToConsole("frame ms: input %.2f emu %.2f blit %.2f overlay %.2f system %.2f",
                                         (double)(fp_t[0] * 10), (double)(fp_t[1] * 10), (double)(fp_t[2] * 10),
                                         (double)(fp_t[3] * 10), (double)(fp_t[4] * 10));
                memset(fp_t, 0, sizeof fp_t);
                fp_frames = 0;
        }
#endif
        if (fatal_msg) {
                draw_fatal();
                return 1;
        }

        unsigned int now = pd->system->getCurrentTimeMilliseconds();
        unsigned int dt_ms = now - last_ms;
        last_ms = now;
        if (dt_ms > 100)
                dt_ms = 100;
        float dt = dt_ms / 1000.0f;

        read_accel();
        if (need_calibrate && --need_calibrate == 0)
                calibrate();

        PDButtons cur, pushed, released;
        pd->system->getButtonState(&cur, &pushed, &released);
        int panel_open = tuning_update(cur, pushed, dt);
#ifdef MARBLE_BENCH
        {
                extern void jit_probe_step(PlaydateAPI *pd);
                static int frames;
                /* The I-cache stress probes run stale code on purpose and
                 * can crash the console: only with MARBLE_BENCH_STRESS.
                 */
#ifdef MARBLE_BENCH_STRESS
                if (++frames > 90 && frames % 20 == 0 && frames < 90 + 20 * 13)
                        jit_probe_step(pd);
#else
                (void)frames;
                (void)jit_probe_step;
#endif
        }
#endif

        if (!mac_ready && mac_booted()) {
                mac_ready = 1;
                /* Start the marble wherever the Mac put its cursor */
                my = (int16_t)RAM_RD16(LM_MOUSE);
                mx = (int16_t)RAM_RD16(LM_MOUSE + 2);
                vx = vy = 0;
                /* (a lander starts upright) */
                angle = tuning_mode() == MODE_LANDER ? -(float)M_PI / 2 : CURSOR_ARROW_ANGLE;
        }
        if (tuning_mode() != cur_mode)
                set_mode(tuning_mode());

#ifdef MARBLE_TIMING
        cur = pushed = 0;
        vx = vy = 0;
        mx = MAC_W / 2;
        my = MAC_H / 2;
#endif
        bungee_input(cur, pushed, panel_open, dt, mx, my, mac_ready, cur_mode == MODE_LANDER);
#ifndef MARBLE_TIMING           /* (there, timing_input() drives the mouse) */
        if (mac_ready) {
                if (cur_mode == MODE_LANDER)
                        step_lander(dt, cur, panel_open);
                else
                        step_marble(dt);
                mac_set_mouse((int)mx, (int)my);
        }
        umac_mouse(0, 0, (cur & kButtonA) ? 1 : 0);
#endif
        clickfx_step(cur & kButtonA, vx, vy, dt);
        FP_MARK(0);

#ifdef MARBLE_TIMING
        timing_run();
#else
        emu_run(dt_ms);
#endif
        FP_MARK(1);

        step_camera(dt);
        blit();
        FP_MARK(2);
        draw_stats(now);
        tuning_draw();
        FP_MARK(3);
#ifdef MARBLE_FRAMEPROF
        fp_last_end = pd->system->getElapsedTime();
#endif
        return 1;
}

static void menu_calibrate(void *ud)
{
        (void)ud;
        need_calibrate = 1;
}

static void menu_stats(void *ud)
{
        show_stats = pd->system->getMenuItemValue((PDMenuItem *)ud);
}

static void init(void)
{
        const char *err;
        ui_font = pd->graphics->loadFont("/System/Fonts/Asheville-Sans-14-Bold.pft", &err);
        if (ui_font)
                pd->graphics->setFont(ui_font);
        tuning_init(pd);
        bungee_init(pd);
        sfx_init(pd);
        /* Unpaced: a frame takes as long as emulation + drawing do.  (A
         * fixed rate made every frame that overran its slot wait for the
         * next one, idling the CPU.)
         */
        pd->display->setRefreshRate(0);
        pd->system->setPeripheralsEnabled(kAccelerometer);

        unsigned int rom_size;
        int dummy;
        mac_rom = load_file(ROM_FILE, &rom_size, &dummy);
        if (!mac_rom) {
                fatal_msg = "No rom.bin found.\n\n"
                        "Put your own Mac Plus v3 ROM\n"
                        "(rom.bin) and boot disk (disk.img) in\n"
                        "Data/local.nick.marblemac/\n"
                        "on the Playdate's data disk.";
                return;
        }
        if (rom_size < ROM_SIZE) {
                fatal_msg = "rom.bin is too small (want 128K Mac Plus ROM)";
                return;
        }
        if (rom_patch(mac_rom)) {
                fatal_msg = "rom.bin isn't a Mac Plus v3 ROM (4D1F8172)";
                return;
        }

        /* No disk is fine: you get the flashing question mark */
        have_disk = open_disk();

        mac_ram = pd->system->realloc(NULL, RAM_SIZE);
        if (!mac_ram) {
                fatal_msg = "Out of memory for Mac RAM";
                return;
        }
        memset(mac_ram, 0, RAM_SIZE);
        /* Pretend this is a warm boot (ROMBase already set up), which
         * makes the ROM skip its RAM pattern test: ~18s saved at 4MB.
         */
        mac_ram[0x2af] = 0x40;

        disc_descr_t discs[DISC_NUM_DRIVES] = { 0 };
        if (have_disk) {
                discs[0].size = vdisk.size;
                discs[0].op_ctx = &vdisk;
                discs[0].op_read = vdisk_read;
                discs[0].op_write = vdisk_write;
        }
        umac_init(mac_ram, mac_rom, discs);
#ifdef MARBLE_BENCH
        {
                /* 4096 NOPs then BRA back: pure dispatch speed */
                extern int m68k_execute(int);
                extern void m68k_set_reg(int, unsigned int);
                extern unsigned long m68k_instruction_count;
                for (int i = 0; i < 4096; i++)
                        RAM_WR16(0x10000 + i * 2, 0x4e71);
                RAM_WR16(0x10000 + 4096 * 2, 0x6000);           /* bra.w */
                RAM_WR16(0x10000 + 4096 * 2 + 2, (uint16_t)(-(4096 * 2 + 2)));
                overlay = 0;
                m68k_set_reg(16 /* M68K_REG_PC */, 0x10000);
                m68k_instruction_count = 0;
                float t0 = pd->system->getElapsedTime();
                m68k_execute(400000);
                float t1 = pd->system->getElapsedTime();
                pd->system->logToConsole("bench: NOP loop %.0f ns/instr (%lu instr)",
                                         (double)((t1 - t0) * 1e9f / m68k_instruction_count), m68k_instruction_count);
                memset(mac_ram, 0, RAM_SIZE);
                mac_ram[0x2af] = 0x40;
                overlay = 1;
                umac_init(mac_ram, mac_rom, discs);
                m68k_instruction_count = 0;
                marble_bench(pd, mac_ram);
        }
#endif

#ifdef UMAC_JIT
        _Static_assert(RAM_SIZE == 4 << 20, "the JIT's inline RAM path assumes a 4MB Mac");
        if (jit_playdate_init(pd, mac_ram, RAM_SIZE, mac_rom, ROM_SIZE))
                pd->system->logToConsole("JIT unavailable, interpreting");
#ifdef MARBLE_BENCH
        {
                /* JIT microbenchmarks: 68k loops at 0x10000 */
                extern void m68k_set_reg(int, unsigned int);
                static const uint16_t reg_loop[] = {
                        0x303C, 0x7FFF,                 /* move.w #$7fff,d0 */
                        0xD481, 0xD682, 0xB781, 0x5281, /* add.l d1,d2; add.l d2,d3; eor.l d3,d1; addq.l #1,d1 */
                        0xD481, 0xD682, 0xB781, 0x5281,
                        0xD481, 0xD682, 0xB781, 0x5281,
                        0xD481, 0xD682, 0xB781, 0x5281,
                        0x51C8, 0xFFDE,                 /* dbra d0,loop */
                        0x60FE,                         /* bra * */
                };
                static const uint16_t mem2[] = {
                        0x2049,                         /* loop: movea.l a1,a0 */
                        0x303C, 0x00FF,                 /* move.w #255,d0 */
                        0x2218, 0xD481,                 /* inner: move.l (a0)+,d1; add.l d1,d2 */
                        0x2218, 0xD481,
                        0x2218, 0xD481,
                        0x2218, 0xD481,
                        0x51C8, 0xFFEE,                 /* dbra d0,inner */
                        0x60E4,                         /* bra loop */
                };
                uint16_t mem3[sizeof mem2 / 2];         /* same, 256KB */
                memcpy(mem3, mem2, sizeof mem2);
                mem3[2] = 0x3FFF;
                struct { const char *name; const uint16_t *code; int n; uint32_t a1; } tests[] = {
                        { "registers", reg_loop, sizeof reg_loop / 2, 0 },
                        { "mem 4KB (cached)", mem2, sizeof mem2 / 2, 0x20000 },
                        { "mem 256KB (misses)", mem3, sizeof mem3 / 2, 0x20000 },
                };
                for (unsigned k = 0; k < sizeof tests / sizeof *tests; k++) {
                        for (int i = 0; i < tests[k].n; i++)
                                RAM_WR16(0x10000 + i * 2, tests[k].code[i]);
                        m68k_jit_note_write(0x10000, tests[k].n * 2);   /* drop old translations */
                        overlay = 0;
                        m68k_set_reg(16, 0x10000);
                        m68k_set_reg(9, tests[k].a1);   /* A1 */
                        m68k_jit_stats.jit_instrs = m68k_jit_stats.interp_instrs = 0;
                        m68k_jit_execute(8 * 1000);     /* warm up: translate */
                        m68k_jit_stats.jit_instrs = m68k_jit_stats.interp_instrs = 0;
                        float t0 = pd->system->getElapsedTime();
                        m68k_jit_execute(8 * 400000);
                        float t1 = pd->system->getElapsedTime();
                        unsigned long n = (unsigned long)(m68k_jit_stats.jit_instrs + m68k_jit_stats.interp_instrs);
                        pd->system->logToConsole("bench: jit %s: %.0f ns/instr (%lu instrs, %lu interp)",
                                                 tests[k].name, (double)((t1 - t0) * 1e9f / n), n,
                                                 (unsigned long)m68k_jit_stats.interp_instrs);
                }
                /* Code footprint: one long straight loop of k register ops,
                 * so the generated code grows past the I-cache.
                 */
                static const int reps[] = { 16, 64, 128, 256, 512 };   /* (bigger trips the 10s watchdog) */
                for (unsigned k = 0; k < sizeof reps / sizeof *reps; k++) {
                        static const uint16_t body[4] = { 0xD481, 0xD682, 0xB781, 0x5281 };
                        uint32_t a = 0x10000;
                        for (int i = 0; i < reps[k]; i++)
                                for (int j = 0; j < 4; j++, a += 2)
                                        RAM_WR16(a, body[j]);
                        RAM_WR16(a, 0x6000);                    /* bra.w 0x10000 */
                        RAM_WR16(a + 2, (uint16_t)(0x10000 - (a + 2)));
                        m68k_jit_note_write(0x10000, a + 4 - 0x10000);
                        overlay = 0;
                        m68k_set_reg(16, 0x10000);
                        uint64_t bytes0 = m68k_jit_stats.code_bytes;
                        m68k_jit_execute(8 * (reps[k] * 4 * 3 + 1000));  /* warm up: translate */
                        uint64_t bytes = m68k_jit_stats.code_bytes - bytes0;
                        m68k_jit_stats.jit_instrs = m68k_jit_stats.interp_instrs = 0;
                        float t0 = pd->system->getElapsedTime();
                        m68k_jit_execute(8 * 400000);
                        float t1 = pd->system->getElapsedTime();
                        unsigned long n = (unsigned long)(m68k_jit_stats.jit_instrs + m68k_jit_stats.interp_instrs);
                        pd->system->logToConsole("bench: jit footprint %d instrs, %lu code bytes: %.0f ns/instr",
                                                 reps[k] * 4 + 1, (unsigned long)bytes, (double)((t1 - t0) * 1e9f / n));
                }
                memset(mac_ram, 0, RAM_SIZE);
                mac_ram[0x2af] = 0x40;
                overlay = 1;
                umac_init(mac_ram, mac_rom, discs);
                m68k_jit_stats.jit_instrs = m68k_jit_stats.interp_instrs = 0;
        }
#endif
#endif

        next_vsync_us = VSYNC_US;
        next_1hz_us = 1000000;
        cam_x = (MAC_W - LCD_COLUMNS) / 2.0f;
        cam_y = (MAC_H - LCD_ROWS) / 2.0f;
        mx = MAC_W / 2;
        my = MAC_H / 2;
        angle = CURSOR_ARROW_ANGLE;
        last_ms = stat_ms0 = pd->system->getCurrentTimeMilliseconds();

        pd->system->addMenuItem("level here", menu_calibrate, NULL);
        PDMenuItem *st = pd->system->addCheckmarkMenuItem("stats", 0, menu_stats, NULL);
        pd->system->setMenuItemUserdata(st, st);
        pd->system->addMenuItem("reset mac", mac_reset, NULL);
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg)
{
        (void)arg;
        switch (event) {
        case kEventInit:
                pd = marble_pd = playdate;
                init();
                pd->system->setUpdateCallback(update, NULL);
                break;
        case kEventLock:
        case kEventTerminate:
                save_disk();
                tuning_save();
                break;
        default:
                break;
        }
        return 0;
}
