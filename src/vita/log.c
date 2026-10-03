/* Tiny file logger. See log.h. */
#include "log.h"
#include "build_info.h"

#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <stdarg.h>
#include <stdio.h>

static FILE *s_log = NULL;

void log_init(void)
{
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir(VITASDR_DATA_DIR, 0777);
    /* Log filename carries the build revision so logs from different builds
     * (e.g. the stable app and this dev app) don't overwrite each other. */
    s_log = fopen(VITASDR_DATA_DIR "/vitasdr-r" VITASDR_BUILD_REV ".log", "w");
    if (s_log) {
        fputs("=== " VITASDR_APP_LABEL " (r" VITASDR_BUILD_REV ") log ===\n",
              s_log);
        fflush(s_log);
    }
}

void vlog(const char *fmt, ...)
{
    if (!s_log)
        return;
    unsigned long ms = (unsigned long)(sceKernelGetProcessTimeWide() / 1000ull);
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(s_log, "[%6lu ms] %s\n", ms, buf);
    fflush(s_log);
}

void log_close(void)
{
    if (s_log) {
        fclose(s_log);
        s_log = NULL;
    }
}
