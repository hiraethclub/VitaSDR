/* Hardware input: buttons and analog sticks. Maps to tuning, volume, squelch,
 * mode/step cycling, and connect/disconnect.
 *
 * Tuning is designed for precision:
 *   D-pad L/R        tune down/up by exactly one step (auto-repeats when held)
 *   D-pad U/D        change the tuning step (1 Hz .. 100 kHz)
 *   Left stick L/R   gentle sweep, grid-snapped, rate scales with deflection
 *   Right stick L/R  volume       Right stick U/D  squelch
 *   L + R together   cycle demodulation mode
 *   Start            connect / disconnect
 *   Select           toggle spectrum
 *   Triangle         open settings
 *   Square           open the band selector
 *   Circle           open the server picker (PS button quits the app)
 * All tuning snaps to the current step grid so it lands on clean frequencies.
 */
#include "app.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

static const int  STEPS[] = { 1, 10, 100, 1000, 5000, 10000, 100000 };
static const int  NSTEPS = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
static const char *MODES[] = { "usb", "lsb", "am", "cw", "nbfm" };
static const int  NMODES = (int)(sizeof(MODES) / sizeof(MODES[0]));

/* Full-deflection analog sweep speed, in tuning steps per second. */
#define SWEEP_MAX_SPS 60.0

static unsigned int s_prev = 0;
static double s_accum = 0.0;   /* fractional-step accumulator for analog sweep */
static int    s_hold = 0;      /* frames a D-pad tune direction has been held */
static int    s_menu_hold = 0; /* frames a menu Up/Down direction has been held */

static int step_index(int step_hz)
{
    for (int i = 0; i < NSTEPS; i++)
        if (STEPS[i] == step_hz)
            return i;
    return 2; /* default 100 Hz */
}

static int mode_index(const char *mode)
{
    for (int i = 0; i < NMODES; i++)
        if (strcmp(MODES[i], mode) == 0)
            return i;
    return 0;
}

/* Move by `steps` tuning steps and snap onto the step grid so we land on clean
 * multiples (e.g. exact kHz). */
static void tune_by(app_state *app, long steps)
{
    if (steps == 0)
        return;
    sceKernelLockMutex(app->lock, 1, NULL);
    long step = app->step_hz > 0 ? app->step_hz : 1;
    long hz = (long)(app->freq_khz * 1000.0 + 0.5);
    hz += steps * step;
    hz = ((hz + step / 2) / step) * step;   /* snap to nearest grid point */
    if (hz < 0) hz = 0;
    /* Upper bound well into VHF so FM/2m etc. are tunable on VHF-capable
     * receivers; a KiwiSDR simply won't receive above ~30 MHz. */
    if (hz > 1800000000L) hz = 1800000000L;
    app->freq_khz = (double)hz / 1000.0;
    sceKernelUnlockMutex(app->lock, 1);
}

void input_init(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    s_prev = 0;
    s_accum = 0.0;
    s_hold = 0;
}

void input_poll(app_state *app)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned int b = pad.buttons;
    unsigned int pressed = b & ~s_prev; /* rising edges */

    /* On the picker/settings/band screens, hand button edges to the menu and
     * skip all radio controls. Up/Down auto-repeat when held so a long list
     * scrolls continuously; Left/Right skip (handled in menu_handle). */
    if (app->screen != SCREEN_RADIO) {
        unsigned int mp = pressed;
        int md = (b & SCE_CTRL_DOWN) ? 1 : ((b & SCE_CTRL_UP) ? -1 : 0);
        if (md != 0) {
            s_menu_hold++;
            if (s_menu_hold > 18 && (s_menu_hold % 3) == 0)
                mp |= (md > 0) ? SCE_CTRL_DOWN : SCE_CTRL_UP; /* repeat ~20/s */
        } else {
            s_menu_hold = 0;
        }
        menu_handle(app, mp);
        s_prev = b;
        return;
    }

    /* Deadzones: generous, because Vita analog sticks rest off-centre. */
    const int dead = 40;    /* left stick (tuning sweep) */
    const int rdead = 55;   /* right stick (volume/squelch) */

    /* ---- D-pad L/R: precise single-step tuning, with hold auto-repeat ---- */
    int dir = (b & SCE_CTRL_RIGHT) ? 1 : ((b & SCE_CTRL_LEFT) ? -1 : 0);
    if (dir != 0) {
        s_hold++;
        int fire = (pressed & (SCE_CTRL_LEFT | SCE_CTRL_RIGHT)) != 0; /* initial */
        if (!fire && s_hold > 20 && (s_hold % 4) == 0)
            fire = 1; /* repeat ~15/s after a ~0.33s delay */
        if (fire)
            tune_by(app, dir);
    } else {
        s_hold = 0;
    }

    /* ---- D-pad U/D: change tuning step ---- */
    if (pressed & SCE_CTRL_UP) {
        int i = step_index(app->step_hz);
        app->step_hz = STEPS[(i + 1) % NSTEPS];
    }
    if (pressed & SCE_CTRL_DOWN) {
        int i = step_index(app->step_hz);
        app->step_hz = STEPS[(i - 1 + NSTEPS) % NSTEPS];
    }

    /* ---- Left stick X: gentle grid-snapped sweep ---- */
    int dx = (int)pad.lx - 128;
    if (dx > dead || dx < -dead) {
        int sign = (dx > 0) ? 1 : -1;
        float norm = (float)(((dx > 0) ? dx : -dx) - dead) / (float)(127 - dead);
        if (norm > 1.0f) norm = 1.0f;
        /* steps/sec grows with the square of deflection: fine near centre,
         * fast at the edge. ~1/60 s per frame. */
        double sps = (double)norm * norm * SWEEP_MAX_SPS;
        s_accum += (double)sign * sps / 60.0;
        long whole = (long)s_accum;
        if (whole != 0) {
            s_accum -= (double)whole;
            tune_by(app, whole);
        }
    } else {
        s_accum = 0.0;
    }

    /* ---- right stick: volume (X), squelch (Y), DOMINANT axis only ----
     * Apply whichever axis is deflected more, so nudging the stick sideways for
     * volume can't accidentally move squelch (and vice versa). */
    int rx = (int)pad.rx - 128;
    int ry = (int)pad.ry - 128;
    int arx = rx < 0 ? -rx : rx;
    int ary = ry < 0 ? -ry : ry;
    if (arx >= ary) {
        if (rx > rdead)      { app->volume += 1; }
        else if (rx < -rdead){ app->volume -= 1; }
    } else {
        if (ry < -rdead)     { app->squelch += 1; }   /* up = increase */
        else if (ry > rdead) { app->squelch -= 1; }
    }
    if (app->volume < 0) app->volume = 0;
    if (app->volume > 100) app->volume = 100;
    if (app->squelch < 0) app->squelch = 0;
    if (app->squelch > 100) app->squelch = 100;

    /* ---- L + R: cycle mode ---- */
    int lr_now = (b & SCE_CTRL_LTRIGGER) && (b & SCE_CTRL_RTRIGGER);
    int lr_prev = (s_prev & SCE_CTRL_LTRIGGER) && (s_prev & SCE_CTRL_RTRIGGER);
    if (lr_now && !lr_prev) {
        int i = mode_index(app->mode);
        const char *nm = MODES[(i + 1) % NMODES];
        sceKernelLockMutex(app->lock, 1, NULL);
        strncpy(app->mode, nm, sizeof(app->mode) - 1);
        app->mode[sizeof(app->mode) - 1] = '\0';
        sceKernelUnlockMutex(app->lock, 1);
        ui_show_message(app, nm);
    }

    /* ---- Start: connect / disconnect ---- */
    if (pressed & SCE_CTRL_START) {
        if (app->conn_status == CONN_CONNECTED ||
            app->conn_status == CONN_CONNECTING)
            app->cmd_disconnect = 1;
        else
            app->cmd_connect = 1;
    }

    /* ---- Select: toggle spectrum ---- */
    if (pressed & SCE_CTRL_SELECT)
        app->show_spectrum = !app->show_spectrum;

    /* ---- Triangle: open settings ---- */
    if (pressed & SCE_CTRL_TRIANGLE)
        app->screen = SCREEN_SETTINGS;

    /* ---- Square: open the band selector ---- */
    if (pressed & SCE_CTRL_SQUARE)
        app->screen = SCREEN_BANDS;

    /* ---- Circle: open the server picker (does NOT quit; use the PS button
     * to exit, so a stray press can't drop the app). ---- */
    if (pressed & SCE_CTRL_CIRCLE)
        app->screen = SCREEN_SERVERS;

    s_prev = b;
}
