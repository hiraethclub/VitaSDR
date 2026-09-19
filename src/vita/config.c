/* Configuration load/save for ux0:data/vitasdr/config.ini.
 *
 * Simple key=value INI. On first run the directory and a commented default
 * file are created so the user can edit in the server details from VitaShell.
 */
#include "app.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_DIR  "ux0:data/vitasdr"
#define CFG_FILE CFG_DIR "/config.ini"

/* A live public KiwiSDR used as the out-of-the-box default so the app connects
 * on first launch. Change `host`/`port` in config.ini to use your own.
 * NOTE: this is a convenience testing default; for a public release we should
 * either make it clearly configurable or point at a receiver intended for
 * heavy public use rather than a personal one. */
#define DEFAULT_HOST "shack2.ddns.net"
#define DEFAULT_PORT 8073

/* Hosts we have shipped as defaults in prior builds. If an existing config
 * still holds one of these (or an empty host), it is upgraded to the current
 * DEFAULT_HOST automatically so updates take effect without hand-editing. A
 * host the user typed themselves is never touched. */
static const char *SHIPPED_DEFAULTS[] = {
    "kiwisdr.example.com",   /* original placeholder */
    "kiwisdr.ucsd.edu"       /* previous default */
};

void config_defaults(app_state *app)
{
    strncpy(app->host, DEFAULT_HOST, sizeof(app->host) - 1);
    app->port = DEFAULT_PORT;
    app->password[0] = '\0';
    app->freq_khz = 7074.0;      /* 40m, 7.074 MHz */
    strncpy(app->mode, "usb", sizeof(app->mode) - 1);
    app->step_hz = 100;
    app->zoom = 9;
    app->volume = 80;
    app->squelch = 0;
    app->palette = 0;
    app->show_spectrum = 1;
}

static void ensure_dir(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(CFG_DIR, 0777);
}

int config_save(const app_state *app)
{
    ensure_dir();
    FILE *f = fopen(CFG_FILE, "w");
    if (!f)
        return -1;
    fprintf(f,
        "# VitaSDR configuration\n"
        "# Set host/port to your KiwiSDR (or any Kiwi-compatible receiver).\n"
        "# Find live public receivers at http://kiwisdr.com/public/\n"
        "# Alternates you can try: canadian-prairies-shortwave.ddns.net:8073\n"
        "host=%s\n"
        "port=%d\n"
        "password=%s\n"
        "freq_khz=%.3f\n"
        "mode=%s\n"
        "step_hz=%d\n"
        "zoom=%d\n"
        "volume=%d\n"
        "squelch=%d\n"
        "palette=%d\n",
        app->host, app->port, app->password, app->freq_khz, app->mode,
        app->step_hz, app->zoom, app->volume, app->squelch, app->palette);
    fclose(f);
    return 0;
}

/* Trim trailing CR/LF/space from a string in place. */
static void rstrip(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
        s[--n] = '\0';
}

int config_load(app_state *app)
{
    config_defaults(app);

    FILE *f = fopen(CFG_FILE, "r");
    if (!f) {
        /* First run: write out the defaults for the user to edit. */
        config_save(app);
        return 1;
    }

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        rstrip(val);

        if (strcmp(key, "host") == 0)
            strncpy(app->host, val, sizeof(app->host) - 1);
        else if (strcmp(key, "port") == 0)
            app->port = atoi(val);
        else if (strcmp(key, "password") == 0)
            strncpy(app->password, val, sizeof(app->password) - 1);
        else if (strcmp(key, "freq_khz") == 0)
            app->freq_khz = atof(val);
        else if (strcmp(key, "mode") == 0)
            strncpy(app->mode, val, sizeof(app->mode) - 1);
        else if (strcmp(key, "step_hz") == 0)
            app->step_hz = atoi(val);
        else if (strcmp(key, "zoom") == 0)
            app->zoom = atoi(val);
        else if (strcmp(key, "volume") == 0)
            app->volume = atoi(val);
        else if (strcmp(key, "squelch") == 0)
            app->squelch = atoi(val);
        else if (strcmp(key, "palette") == 0)
            app->palette = atoi(val);
    }
    fclose(f);

    /* Upgrade an existing config that still holds a previously-shipped default
     * (or an empty host) to the current default, and persist it, so the app
     * connects without the user having to hand-edit anything. A host the user
     * chose themselves is left alone. */
    int is_shipped = (app->host[0] == '\0');
    for (size_t i = 0; !is_shipped &&
                       i < sizeof(SHIPPED_DEFAULTS) / sizeof(SHIPPED_DEFAULTS[0]);
         i++) {
        if (strcmp(app->host, SHIPPED_DEFAULTS[i]) == 0)
            is_shipped = 1;
    }
    if (is_shipped) {
        strncpy(app->host, DEFAULT_HOST, sizeof(app->host) - 1);
        app->host[sizeof(app->host) - 1] = '\0';
        if (app->port == 0)
            app->port = DEFAULT_PORT;
        config_save(app);
    }
    return 0;
}
