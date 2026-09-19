/* Hardware input: buttons and analog sticks. Maps to tuning, volume, squelch,
 * mode/step cycling, and connect/disconnect. A subset of the full remappable
 * scheme; the defaults that matter for basic operation.
 *
 * Controls:
 *   Left stick L/R   tune (accelerated: gentle = fine, full = fast)
 *   D-pad L/R        cycle tuning step
 *   Right stick L/R  volume      Right stick U/D  squelch
 *   L + R together   cycle demodulation mode
 *   Start            connect / disconnect
 *   Select           toggle spectrum
 *   Circle           exit
 */
#include "app.h"

#include <psp2/ctrl.h>
#include <psp2/kernel/threadmgr.h>

#include <string.h>

static const int  STEPS[] = { 1, 10, 100, 1000, 5000, 10000, 100000 };
static const int  NSTEPS = (int)(sizeof(STEPS) / sizeof(STEPS[0]));
static const char *MODES[] = { "usb", "lsb", "am", "cw", "nbfm" };
static const int  NMODES = (int)(sizeof(MODES) / sizeof(MODES[0]));

static unsigned int s_prev = 0;

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

void input_init(void)
{
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    s_prev = 0;
}

void input_poll(app_state *app)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);
    unsigned int b = pad.buttons;
    unsigned int pressed = b & ~s_prev; /* rising edges */

    /* Deadzones: generous, because Vita analog sticks rest off-centre and a
     * small resting drift was silently retuning / draining the volume. */
    const int dead = 40;    /* left stick (tuning) */
    const int rdead = 55;   /* right stick (volume/squelch), needs more margin */

    /* ---- analog tuning (left stick X) ---- */
    int dx = (int)pad.lx - 128;
    if (dx > dead || dx < -dead) {
        int sign = (dx > 0) ? 1 : -1;
        float norm = (float)(((dx > 0) ? dx : -dx) - dead) / (float)(127 - dead);
        if (norm > 1.0f) norm = 1.0f;
        long delta_hz = (long)(app->step_hz + norm * norm * 20000.0f);
        sceKernelLockMutex(app->lock, 1, NULL);
        app->freq_khz += (double)(sign * delta_hz) / 1000.0;
        if (app->freq_khz < 0) app->freq_khz = 0;
        if (app->freq_khz > 30000.0) app->freq_khz = 30000.0;
        sceKernelUnlockMutex(app->lock, 1);
    }

    /* ---- right stick: volume (X), squelch (Y) ---- */
    int rx = (int)pad.rx - 128;
    if (rx > rdead)      { app->volume += 1; }
    else if (rx < -rdead){ app->volume -= 1; }
    if (app->volume < 0) app->volume = 0;
    if (app->volume > 100) app->volume = 100;

    int ry = (int)pad.ry - 128;
    if (ry < -rdead)     { app->squelch += 1; }   /* up = increase */
    else if (ry > rdead) { app->squelch -= 1; }
    if (app->squelch < 0) app->squelch = 0;
    if (app->squelch > 100) app->squelch = 100;

    /* ---- D-pad L/R: tuning step ---- */
    if (pressed & SCE_CTRL_RIGHT) {
        int i = step_index(app->step_hz);
        app->step_hz = STEPS[(i + 1) % NSTEPS];
    }
    if (pressed & SCE_CTRL_LEFT) {
        int i = step_index(app->step_hz);
        app->step_hz = STEPS[(i - 1 + NSTEPS) % NSTEPS];
    }

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

    /* ---- Circle: exit ---- */
    if (pressed & SCE_CTRL_CIRCLE)
        app->running = 0;

    s_prev = b;
}
