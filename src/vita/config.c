/* Configuration load/save for ux0:data/vitasdr/config.ini.
 *
 * Simple key=value INI. On first run the directory and a commented default
 * file are created so the user can edit in the server details from VitaShell.
 */
#include "app.h"
#include "build_info.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CFG_DIR  VITASDR_DATA_DIR
#define CFG_FILE CFG_DIR "/config.ini"

/* No baked-in receiver: on first launch the app shows the server picker with
 * the live public directory, and the chosen receiver becomes the saved host.
 * (A personal test receiver used to be shipped here; removed for release.) */
#define DEFAULT_HOST ""
#define DEFAULT_PORT 8073

/* Hosts we have shipped as defaults in prior builds. If an existing config
 * still holds one of these (or an empty host), it is reset to the current
 * DEFAULT_HOST automatically so updates take effect without hand-editing. A
 * host the user typed themselves is never touched. */
static const char *SHIPPED_DEFAULTS[] = {
    "kiwisdr.example.com",        /* original placeholder */
    "kiwisdr.ucsd.edu",           /* earlier default */
    "shack2.ddns.net",            /* previous default */
    "gw0kax.proxy.kiwisdr.com"    /* v0.2.x reference receiver */
};

void config_defaults(app_state *app)
{
    strncpy(app->host, DEFAULT_HOST, sizeof(app->host) - 1);
    app->port = DEFAULT_PORT;
    app->password[0] = '\0';
    app->proto = PROTO_KIWI;
    app->path[0] = '\0';
    app->tls = 0;
    app->tls_verify = 1;   /* verify certificates by default; toggle in Settings */
    app->freq_khz = 7074.0;      /* 40m, 7.074 MHz */
    strncpy(app->mode, "usb", sizeof(app->mode) - 1);
    app->step_hz = 100;
    app->zoom = 9;
    app->volume = 80;
    app->squelch = 0;
    app->palette = 0;
    app->show_spectrum = 1;

    /* QoL settings */
    app->wf_speed = 4;
    app->audio_bw = AUDIO_BW_NORMAL;
    app->auto_connect = 0;    /* default: show the server picker on launch */
    app->auto_reconnect = 1;
    app->keep_awake = 1;

    /* Server-picker filters. Hide receivers with no room for our two
     * connections by default; everything else off until the user asks. */
    app->flt_free2 = 1;
    app->flt_min_snr = 0;
    app->flt_loc[0] = '\0';
    app->flt_dist_km = 0;
    app->home_set = 0;
    app->home_lat = 0.0f;
    app->home_lon = 0.0f;
    app->flt_sort_best = 1;
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
        "proto=%d\n"
        "path=%s\n"
        "tls=%d\n"
        "tls_verify=%d\n"
        "freq_khz=%.3f\n"
        "mode=%s\n"
        "step_hz=%d\n"
        "zoom=%d\n"
        "volume=%d\n"
        "squelch=%d\n"
        "palette=%d\n"
        "wf_speed=%d\n"
        "audio_bw=%d\n"
        "auto_connect=%d\n"
        "auto_reconnect=%d\n"
        "keep_awake=%d\n"
        "flt_free2=%d\n"
        "flt_min_snr=%d\n"
        "flt_loc=%s\n"
        "flt_dist_km=%d\n"
        "home_set=%d\n"
        "home_lat=%.5f\n"
        "home_lon=%.5f\n"
        "flt_sort_best=%d\n",
        app->host, app->port, app->password, app->proto, app->path,
        app->tls, app->tls_verify,
        app->freq_khz, app->mode,
        app->step_hz, app->zoom, app->volume, app->squelch, app->palette,
        app->wf_speed, app->audio_bw, app->auto_connect, app->auto_reconnect,
        app->keep_awake,
        app->flt_free2, app->flt_min_snr, app->flt_loc, app->flt_dist_km,
        app->home_set, (double)app->home_lat, (double)app->home_lon,
        app->flt_sort_best);
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
        else if (strcmp(key, "proto") == 0)
            app->proto = atoi(val);
        else if (strcmp(key, "path") == 0)
            strncpy(app->path, val, sizeof(app->path) - 1);
        else if (strcmp(key, "tls") == 0)
            app->tls = atoi(val);
        else if (strcmp(key, "tls_verify") == 0)
            app->tls_verify = atoi(val);
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
        else if (strcmp(key, "wf_speed") == 0)
            app->wf_speed = atoi(val);
        else if (strcmp(key, "audio_bw") == 0)
            app->audio_bw = atoi(val);
        else if (strcmp(key, "auto_connect") == 0)
            app->auto_connect = atoi(val);
        else if (strcmp(key, "auto_reconnect") == 0)
            app->auto_reconnect = atoi(val);
        else if (strcmp(key, "keep_awake") == 0)
            app->keep_awake = atoi(val);
        else if (strcmp(key, "flt_free2") == 0)
            app->flt_free2 = atoi(val);
        else if (strcmp(key, "flt_min_snr") == 0)
            app->flt_min_snr = atoi(val);
        else if (strcmp(key, "flt_loc") == 0)
            strncpy(app->flt_loc, val, sizeof(app->flt_loc) - 1);
        else if (strcmp(key, "flt_dist_km") == 0)
            app->flt_dist_km = atoi(val);
        else if (strcmp(key, "home_set") == 0)
            app->home_set = atoi(val);
        else if (strcmp(key, "home_lat") == 0)
            app->home_lat = (float)atof(val);
        else if (strcmp(key, "home_lon") == 0)
            app->home_lon = (float)atof(val);
        else if (strcmp(key, "flt_sort_best") == 0)
            app->flt_sort_best = atoi(val);
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
        app->proto = PROTO_KIWI;   /* shipped defaults are all KiwiSDR */
        app->path[0] = '\0';
        app->tls = 0;
        config_save(app);
    }
    return 0;
}
