/* HF band plan lookup. See bandplan.h.
 *
 * Ranges are in kHz, ordered low to high; the first containing range wins.
 * Covers the LW/MW broadcast bands, the HF amateur bands, the international
 * shortwave broadcast bands (by metre band), plus CB. Region-1/2/3 amateur
 * edges differ slightly; the widest common allocation is used so a signal is
 * never mislabelled as "outside" a band it is really in.
 */
#include "bandplan.h"

typedef struct {
    double lo_khz;
    double hi_khz;
    const char *name;
    double tune_khz;     /* a sensible place to land when jumping to this band */
    const char *mode;    /* mode to use there */
} band_entry;

static const band_entry BANDS[] = {
    {   148.5,   283.5, "LW Broadcast",   198.0,  "am"  },
    {   530.0,  1710.0, "MW Broadcast",   999.0,  "am"  },
    {  1810.0,  2000.0, "160m Amateur",  1900.0,  "lsb" },
    {  2300.0,  2495.0, "120m SWBC",     2400.0,  "am"  },
    {  3200.0,  3400.0, "90m SWBC",      3300.0,  "am"  },
    {  3500.0,  4000.0, "80m Amateur",   3700.0,  "lsb" },
    {  3900.0,  4000.0, "75m SWBC",      3950.0,  "am"  },
    {  4750.0,  5060.0, "60m SWBC",      4900.0,  "am"  },
    {  5250.0,  5450.0, "60m Amateur",   5357.0,  "usb" },
    {  5900.0,  6200.0, "49m SWBC",      6100.0,  "am"  },
    {  7000.0,  7300.0, "40m Amateur",   7100.0,  "lsb" },
    {  7200.0,  7450.0, "41m SWBC",      7300.0,  "am"  },
    {  9400.0,  9900.0, "31m SWBC",      9600.0,  "am"  },
    { 10100.0, 10150.0, "30m Amateur",  10120.0,  "usb" },
    { 11600.0, 12100.0, "25m SWBC",     11800.0,  "am"  },
    { 13570.0, 13870.0, "22m SWBC",     13700.0,  "am"  },
    { 14000.0, 14350.0, "20m Amateur",  14200.0,  "usb" },
    { 15100.0, 15830.0, "19m SWBC",     15400.0,  "am"  },
    { 17480.0, 17900.0, "16m SWBC",     17700.0,  "am"  },
    { 18068.0, 18168.0, "17m Amateur",  18130.0,  "usb" },
    { 21000.0, 21450.0, "15m Amateur",  21300.0,  "usb" },
    { 21450.0, 21850.0, "13m SWBC",     21600.0,  "am"  },
    { 24890.0, 24990.0, "12m Amateur",  24950.0,  "usb" },
    { 25670.0, 26100.0, "11m SWBC",     25800.0,  "am"  },
    { 26965.0, 27405.0, "CB",           27185.0,  "am"  },
    { 28000.0, 29700.0, "10m Amateur",  28400.0,  "usb" },
    /* HF only (<= 30 MHz): everything here is reachable by a KiwiSDR. VHF bands
     * need a VHF-capable receiver via OpenWebRX, so they are omitted for now.
     * Re-add when OWRX + your own gear are wired up:
     *   {  50000.0,  54000.0, "6m Amateur",   50150.0,  "usb"  },
     *   {  87500.0, 108000.0, "FM Broadcast", 98000.0,  "wfm"  },
     *   { 144000.0, 148000.0, "2m Amateur",  145000.0,  "nbfm" }, */
};

#define NBANDS (int)(sizeof(BANDS) / sizeof(BANDS[0]))

const char *band_lookup(double freq_khz)
{
    for (int i = 0; i < NBANDS; i++) {
        if (freq_khz >= BANDS[i].lo_khz && freq_khz <= BANDS[i].hi_khz)
            return BANDS[i].name;
    }
    return "";
}

int bandplan_count(void) { return NBANDS; }

void bandplan_get(int i, const char **name, double *tune_khz, const char **mode)
{
    if (i < 0 || i >= NBANDS) {
        if (name) *name = "";
        if (tune_khz) *tune_khz = 0.0;
        if (mode) *mode = "am";
        return;
    }
    if (name) *name = BANDS[i].name;
    if (tune_khz) *tune_khz = BANDS[i].tune_khz;
    if (mode) *mode = BANDS[i].mode;
}
