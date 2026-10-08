/* Wall sounds: thunks when the marble hits an edge of the Mac screen, and
 * a continuous scrape while it slides along one.
 *
 * One mono callback source renders everything at 44.1kHz:
 *
 *  - Thunks: up to 4 one-shot voices.  Each is a sine "body" whose pitch
 *    drops fast (a struck, hollow-ish thing), an optional inharmonic
 *    second partial (woodiness), and a short burst of low-passed noise for
 *    the contact click, through a soft saturator that gets more drive the
 *    harder the hit, so big hits are punchier rather than just louder.
 *    There are a few presets, picked at random, with pitch jitter.  The
 *    Playdate's speaker has next to nothing below ~250Hz, so the bodies
 *    start a few hundred Hz up and rely on the click and saturation
 *    harmonics to read as "thunk".
 *
 *  - Scrape: a single, always-running voice (never retriggered): white
 *    noise through a band-pass whose centre rises with sliding speed,
 *    with a random "grain" amplitude wobble and sparse crackles so it
 *    sounds gritty rather than like hiss.  Its gain follows a target the
 *    game sets each frame, with a fast-ish attack and slower release.
 *
 *  - Mouse button: pressing it plays a "thump", a darker, duller cousin of
 *    the thunks rendered by the same voices (low body with a quick pitch
 *    drop, a dull two-pole low-passed noise puff, gentle saturation), and
 *    releasing it an even quieter, shorter "tup".
 *
 *  - Drag: a second always-running rustle for moving the cursor with the
 *    button held.  Like the scrape but darker and smoother: a lower
 *    band-pass, slower and shallower grain, no crackles, and softer
 *    attack/release.  It's ducked while the wall scrape is sounding so the
 *    two together don't pile up into the clipper.
 *
 * Everything the game thread sets is either a single 32-bit word or goes
 * through a small single-producer/single-consumer ring, so there's no
 * locking in the audio callback.
 */

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include "sfx.h"

#define SR              44100.0f
#define NVOICES         4
#define RING            8               /* power of 2 */

#define MASTER_GAIN     0.55f           /* mix -> soft clipper input */
#define OUT_SCALE       30000.0f        /* clipper peaks at 2/3 -> ~-4dBFS */

/* Scrape envelope (s) and timeout if the game stops updating it */
#define RUSTLE_ATTACK   0.020f
#define RUSTLE_RELEASE  0.090f
#define RUSTLE_STALE    0.25f
#define RUSTLE_GAIN     0.75f

/* Drag rustle: as above, but slower to come and go, and ducked by up to
 * DRAG_DUCK while the scrape plays */
#define DRAG_ATTACK     0.040f
#define DRAG_RELEASE    0.150f
#define DRAG_GAIN       0.80f
#define DRAG_DUCK       0.45f

////////////////////////////////////////////////////////////////////////////////
// Shared state

/* A thunk, fully worked out on the game thread (the expf()s live here, not
 * in the callback) */
typedef struct {
        float amp;
        float f_end, f_sweep;   /* body pitch = f_end + f_sweep * penv */
        float kp;               /* per-sample decay of the pitch sweep */
        float kb;               /* per-sample decay of the body */
        float p2_ratio, p2_amp; /* second partial */
        float n_amp, kn, n_lp;  /* noise burst: level, decay, one-pole coef */
        float n_lp2;            /* second one-pole (1 = bypass) */
        float drive;
        float out;              /* 1/sat(drive), times the voice's volume */
} thunk_t;

static thunk_t ring[RING];
static atomic_uint ring_head;   /* written by the game */
static atomic_uint ring_tail;   /* written by the audio callback */

static volatile float master_vol = 0.7f;
static volatile float click_vol = 0.7f;
static volatile float rustle_target;
static atomic_uint rustle_seq;  /* bumped on every sfx_rustle() */
static volatile float drag_target;
static atomic_uint drag_seq;    /* bumped on every sfx_drag() */

static uint32_t game_rng = 0x2545F491u;
static int last_preset = -1;

////////////////////////////////////////////////////////////////////////////////
// Audio-thread state

typedef struct {
        int active;
        thunk_t t;
        float penv, benv, nenv;
        float ph1, ph2, lp, lp2;
} voice_t;

static voice_t voices[NVOICES];
static uint32_t audio_rng = 0x9E3779B9u;

static float r_gain, r_level;           /* smoothed scrape gain and level */
static float r_low, r_band;             /* state-variable filter */
static float r_f = 0.1f;
static float r_grain = 1, r_grain_t = 1;
static float r_crack;
static int r_grain_n;
static unsigned int r_seen_seq, r_stale_samples;

static float d_gain, d_level;           /* drag rustle, as above */
static float d_low, d_band;
static float d_f = 0.05f;
static float d_grain = 1, d_grain_t = 1;
static int d_grain_n;
static unsigned int d_seen_seq, d_stale_samples;

/* Envelope coefficients, worked out once in sfx_init() */
static float ka_r, kr_r, ka_d, kr_d;

static inline uint32_t xorshift(uint32_t *s)
{
        uint32_t x = *s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        return *s = x;
}

/* -1..1 */
static inline float noise(uint32_t *s)
{
        return (int32_t)xorshift(s) * (1.0f / 2147483648.0f);
}

/* 0..1 */
static inline float frand(uint32_t *s)
{
        return (xorshift(s) >> 8) * (1.0f / 16777216.0f);
}

/* Parabolic sine of 2*pi*p, p in [0,1).  A couple of percent of odd
 * harmonics, which on this speaker is a feature.
 */
static inline float psin(float p)
{
        return p < 0.5f ? 16.0f * p * (0.5f - p) : -16.0f * (p - 0.5f) * (1.0f - p);
}

static inline float sat(float x)
{
        return x / (1.0f + fabsf(x));
}

////////////////////////////////////////////////////////////////////////////////
// Rendering

static void take_thunks(void)
{
        unsigned int head = atomic_load_explicit(&ring_head, memory_order_acquire);
        unsigned int tail = atomic_load_explicit(&ring_tail, memory_order_relaxed);
        while (tail != head) {
                /* Free voice, else steal the most decayed one */
                voice_t *v = &voices[0];
                for (int i = 0; i < NVOICES; i++) {
                        if (!voices[i].active) {
                                v = &voices[i];
                                break;
                        }
                        if (voices[i].benv < v->benv)
                                v = &voices[i];
                }
                v->t = ring[tail % RING];
                v->active = 1;
                v->penv = v->benv = v->nenv = 1;
                v->ph1 = v->ph2 = 0;
                v->lp = v->lp2 = 0;
                tail++;
        }
        atomic_store_explicit(&ring_tail, tail, memory_order_release);
}

static float render_voice(voice_t *v)
{
        thunk_t *t = &v->t;
        float f = t->f_end + t->f_sweep * v->penv;
        v->penv *= t->kp;
        v->ph1 += f * (1.0f / SR);
        if (v->ph1 >= 1.0f)
                v->ph1 -= 1.0f;
        v->ph2 += f * t->p2_ratio * (1.0f / SR);
        if (v->ph2 >= 1.0f)
                v->ph2 -= 1.0f;
        /* the second partial dies twice as fast (squared envelope) */
        float body = (psin(v->ph1) + t->p2_amp * psin(v->ph2) * v->benv) * v->benv;
        v->benv *= t->kb;

        v->lp += (noise(&audio_rng) - v->lp) * t->n_lp;
        v->lp2 += (v->lp - v->lp2) * t->n_lp2;
        float click = v->lp2 * v->nenv * t->n_amp;
        v->nenv *= t->kn;

        if (v->benv < 0.0003f && v->nenv < 0.0003f)
                v->active = 0;
        return sat(t->drive * t->amp * (body + click)) * t->out;
}

/* Has the game updated this continuous voice's target recently? */
static int fresh(atomic_uint *seqp, unsigned int *seen, unsigned int *stale, int len)
{
        const unsigned int limit = (unsigned int)(RUSTLE_STALE * SR);
        unsigned int seq = atomic_load_explicit(seqp, memory_order_relaxed);
        if (seq != *seen) {
                *seen = seq;
                *stale = 0;
        } else if (*stale < limit) {
                *stale += len;
        }
        return *stale < limit;
}

static int render(void *ctx, int16_t *left, int16_t *right, int len)
{
        (void)ctx;
        take_thunks();

        /* Continuous targets, each forced to zero if the game has gone quiet */
        float target = fresh(&rustle_seq, &r_seen_seq, &r_stale_samples, len) ? rustle_target : 0;
        float dtarget = fresh(&drag_seq, &d_seen_seq, &d_stale_samples, len) ? drag_target : 0;
        /* Volumes: thunks and thumps carry their own (set when posted),
         * the rustles get theirs here */
        float vol = master_vol, cvol = click_vol;
        if (vol == 0)
                target = 0;
        if (cvol == 0)
                dtarget = 0;

        int any_voice = 0;
        for (int i = 0; i < NVOICES; i++)
                any_voice |= voices[i].active;
        int scrape_on = target > 0 || r_gain >= 1e-4f;
        int drag_on = dtarget > 0 || d_gain >= 1e-3f;   /* slow tail: cut at -60dB */
        if (!scrape_on)
                r_gain = 0;
        if (!drag_on)
                d_gain = 0;
        if (!any_voice && !scrape_on && !drag_on)
                return 0;

        /* Per-block: filter centres follow the (smoothed) levels */
        const float ka = ka_r, kr = kr_r;
        float fc = 650.0f + 2000.0f * r_level;
        float f_target = 2.0f * sinf((float)M_PI * fc / SR);
        const float q = 0.9f;           /* damping; band gain at fc = 1/q */
        float crack_p = 0.0025f * r_level;

        float dfc = 330.0f + 650.0f * d_level;
        float df_target = 2.0f * sinf((float)M_PI * dfc / SR);
        const float dq = 1.0f;
        /* ...and the drag rustle ducks under the scrape */
        const float d_k = DRAG_GAIN * dq * cvol, duck_k = DRAG_DUCK * vol;

        for (int n = 0; n < len; n++) {
                float x = 0;
                if (any_voice) {
                        for (int i = 0; i < NVOICES; i++)
                                if (voices[i].active)
                                        x += render_voice(&voices[i]);
                }

                if (scrape_on) {
                        r_gain += (target - r_gain) * (target > r_gain ? ka : kr);
                        r_level += (target - r_level) * kr;
                        r_f += (f_target - r_f) * 0.002f;
                        if (--r_grain_n <= 0) {
                                /* new grain every 1-3ms */
                                r_grain_n = 40 + (xorshift(&audio_rng) & 63);
                                r_grain_t = 0.35f + 0.65f * frand(&audio_rng);
                        }
                        r_grain += (r_grain_t - r_grain) * 0.03f;
                        if (frand(&audio_rng) < crack_p)
                                r_crack = 0.15f + 0.3f * frand(&audio_rng);
                        float w = noise(&audio_rng);
                        r_low += r_f * r_band;
                        float high = w - r_low - q * r_band;
                        r_band += r_f * high;
                        float s = r_band * q * RUSTLE_GAIN * r_grain + w * r_crack;
                        r_crack *= 0.82f;
                        x += s * (r_gain * vol);
                }

                if (drag_on) {
                        d_gain += (dtarget - d_gain) * (dtarget > d_gain ? ka_d : kr_d);
                        d_level += (dtarget - d_level) * kr_d;
                        d_f += (df_target - d_f) * 0.0015f;
                        if (--d_grain_n <= 0) {
                                /* longer, shallower grains than the scrape: 3-6ms */
                                d_grain_n = 130 + (xorshift(&audio_rng) & 127);
                                d_grain_t = 0.6f + 0.4f * frand(&audio_rng);
                        }
                        d_grain += (d_grain_t - d_grain) * 0.01f;
                        float w = noise(&audio_rng);
                        d_low += d_f * d_band;
                        float high = w - d_low - dq * d_band;
                        d_band += d_f * high;
                        x += d_band * d_k * d_grain * d_gain * (1.0f - duck_k * r_gain);
                }

                /* Cubic soft clip: linear-ish to 0.5, flat at 2/3 */
                x *= MASTER_GAIN;
                if (x > 1.0f)
                        x = 1.0f;
                else if (x < -1.0f)
                        x = -1.0f;
                x -= x * x * x * (1.0f / 3.0f);
                int16_t o = (int16_t)(x * OUT_SCALE);
                left[n] = o;
                if (right)
                        right[n] = o;
        }
        return 1;
}

////////////////////////////////////////////////////////////////////////////////
// Game thread

typedef struct {
        float f0, f1;           /* start/end pitch, Hz */
        float sweep_ms;         /* pitch drop time constant */
        float body_ms;          /* body decay time constant */
        float p2_ratio, p2_amp;
        float n_amp, n_ms, n_hz;
        float drive;
} preset_t;

static const preset_t presets[] = {
        /* knock: woody, a bit hollow */
        { 520, 190, 12, 45,  2.71f, 0.30f, 2.2f, 6,  3000, 1.2f },
        /* thud: dull and low, more squashed */
        { 340, 125, 18, 65,  0,     0,     1.6f, 10, 1300, 2.0f },
        /* tock: small plastic box */
        { 880, 430, 8,  28,  1.52f, 0.30f, 2.6f, 4,  5000, 1.0f },
        /* bonk: boxy, rings a little */
        { 430, 165, 25, 85,  2.03f, 0.22f, 1.5f, 8,  2000, 1.6f },
};
#define NPRESETS ((int)(sizeof presets / sizeof *presets))

static float per_sample(float ms)
{
        return expf(-1000.0f / (ms * SR));
}

/* Fill in the derived parts of a thunk_t and queue it */
static void post(thunk_t *t, float n_hz, float n_hz2, float vol)
{
        t->n_lp = 1.0f - expf(-2.0f * (float)M_PI * n_hz / SR);
        t->n_lp2 = n_hz2 > 0 ? 1.0f - expf(-2.0f * (float)M_PI * n_hz2 / SR) : 1.0f;
        t->out = vol * (1.0f + t->drive) / t->drive;    /* sat(drive) -> vol */
        unsigned int head = atomic_load_explicit(&ring_head, memory_order_relaxed);
        ring[head % RING] = *t;
        atomic_store_explicit(&ring_head, head + 1, memory_order_release);
}

static int ring_full(void)
{
        unsigned int head = atomic_load_explicit(&ring_head, memory_order_relaxed);
        unsigned int tail = atomic_load_explicit(&ring_tail, memory_order_acquire);
        return head - tail >= RING;
}

void sfx_thunk(float h)
{
        h = fmaxf(0, fminf(1, h));
        float vol = master_vol;
        if (vol == 0 || ring_full())
                return;

        /* Random preset, never the same twice running */
        int p = xorshift(&game_rng) % NPRESETS;
        if (p == last_preset)
                p = (p + 1 + xorshift(&game_rng) % (NPRESETS - 1)) % NPRESETS;
        last_preset = p;
        const preset_t *ps = &presets[p];

        float jitter = 0.94f + 0.12f * frand(&game_rng);
        thunk_t t;
        /* Soft touches are quiet, dull and short; hard ones loud, bright,
         * higher-starting, longer and more driven.
         */
        t.amp = 0.12f + 0.88f * powf(h, 1.2f);
        float f0 = ps->f0 * jitter * (0.9f + 0.35f * h);
        t.f_end = ps->f1 * jitter;
        t.f_sweep = fmaxf(0, f0 - t.f_end);
        t.kp = per_sample(ps->sweep_ms * (0.8f + 0.4f * h));
        t.kb = per_sample(ps->body_ms * (0.7f + 0.6f * h));
        t.p2_ratio = ps->p2_ratio;
        t.p2_amp = ps->p2_amp;
        t.n_amp = ps->n_amp * (0.6f + 0.6f * h);
        t.kn = per_sample(ps->n_ms);
        t.drive = ps->drive * (0.6f + 1.4f * h);
        post(&t, ps->n_hz * (0.5f + 0.8f * h), 0, vol);
}

/* The mouse button thump.  Compared with the thunks: lower and with a
 * faster, deeper pitch drop (the "weight" is mostly the first ~10ms
 * sweeping down through the speaker's range), no bright second partial,
 * a longer, much duller noise puff through two low-pass poles, and just
 * enough drive to round it off and add a little 3rd harmonic.  Peaks at
 * THUMP_LEVEL, well under a hard wall thunk.
 */
#define THUMP_LEVEL     0.70f
#define TUP_LEVEL       0.16f

void sfx_click(void)
{
        float vol = click_vol;
        if (vol == 0 || ring_full())
                return;
        float jitter = 0.95f + 0.10f * frand(&game_rng);
        thunk_t t = {
                .amp = 0.9f,
                .f_end = 120.0f * jitter,
                .f_sweep = (420.0f - 120.0f) * jitter,
                .kp = per_sample(12),
                .kb = per_sample(35),
                .n_amp = 1.5f,
                .kn = per_sample(10),
                .drive = 2.0f,
        };
        post(&t, 550.0f * jitter, 400.0f, vol * THUMP_LEVEL);
}

/* ...and the release: a small, short, damped "tup", a little higher */
void sfx_unclick(void)
{
        float vol = click_vol;
        if (vol == 0 || ring_full())
                return;
        float jitter = 0.95f + 0.10f * frand(&game_rng);
        thunk_t t = {
                .amp = 0.8f,
                .f_end = 200.0f * jitter,
                .f_sweep = (460.0f - 200.0f) * jitter,
                .kp = per_sample(4),
                .kb = per_sample(12),
                .n_amp = 1.0f,
                .kn = per_sample(4),
                .drive = 1.2f,
        };
        post(&t, 1000.0f * jitter, 700.0f, vol * TUP_LEVEL);
}

void sfx_rustle(float level)
{
        rustle_target = fmaxf(0, fminf(1, level));
        atomic_fetch_add_explicit(&rustle_seq, 1, memory_order_relaxed);
}

void sfx_drag(float level)
{
        drag_target = fmaxf(0, fminf(1, level));
        atomic_fetch_add_explicit(&drag_seq, 1, memory_order_relaxed);
}

void sfx_set_volume(float vol)
{
        master_vol = fmaxf(0, fminf(1, vol));
}

void sfx_set_click_volume(float vol)
{
        click_vol = fmaxf(0, fminf(1, vol));
}

static void init_coefs(void)
{
        ka_r = 1.0f - expf(-1.0f / (RUSTLE_ATTACK * SR));
        kr_r = 1.0f - expf(-1.0f / (RUSTLE_RELEASE * SR));
        ka_d = 1.0f - expf(-1.0f / (DRAG_ATTACK * SR));
        kr_d = 1.0f - expf(-1.0f / (DRAG_RELEASE * SR));
}

void sfx_init(PlaydateAPI *pd)
{
        game_rng ^= pd->system->getCurrentTimeMilliseconds() * 2654435761u;
        if (!game_rng)
                game_rng = 1;
        init_coefs();
        pd->sound->addSource(render, NULL, 0);
}
