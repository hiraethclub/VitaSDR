/* HF band plan: map a frequency to a short human label so the user knows what
 * to expect (amateur bands, shortwave broadcast bands, etc.). Portable.
 */
#ifndef VITASDR_BANDPLAN_H
#define VITASDR_BANDPLAN_H

/* Return a short label for the band containing freq_khz, or "" if none. The
 * returned pointer is a static string, valid for the program's lifetime. */
const char *band_lookup(double freq_khz);

/* Band list for the band-jump selector. */
int  bandplan_count(void);
/* Fetch band i: its label, a representative tune frequency (kHz) and the mode
 * to use ("am"/"lsb"/"usb"/...). Pointers are static strings. */
void bandplan_get(int i, const char **name, double *tune_khz, const char **mode);

#endif /* VITASDR_BANDPLAN_H */
