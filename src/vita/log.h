/* Tiny file logger for on-device diagnostics.
 *
 * Writes to ux0:data/vitasdr/vitasdr.log, truncated at each launch, flushed
 * after every line so the log survives a crash. Vita-only. */
#ifndef VITASDR_LOG_H
#define VITASDR_LOG_H

void log_init(void);
void vlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void log_close(void);

#endif /* VITASDR_LOG_H */
