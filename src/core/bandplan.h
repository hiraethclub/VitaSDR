/* HF band plan: map a frequency to a short human label so the user knows what
 * to expect (amateur bands, shortwave broadcast bands, etc.). Portable.
 */
#ifndef VITASDR_BANDPLAN_H
#define VITASDR_BANDPLAN_H

/* Return a short label for the band containing freq_khz, or "" if none. The
 * returned pointer is a static string, valid for the program's lifetime. */
const char *band_lookup(double freq_khz);

#endif /* VITASDR_BANDPLAN_H */
