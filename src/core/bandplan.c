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
} band_entry;

static const band_entry BANDS[] = {
    {   148.5,   283.5, "LW Broadcast" },
    {   530.0,  1710.0, "MW Broadcast" },
    {  1810.0,  2000.0, "160m Amateur" },
    {  2300.0,  2495.0, "120m SWBC" },
    {  3200.0,  3400.0, "90m SWBC" },
    {  3500.0,  4000.0, "80m Amateur" },
    {  3900.0,  4000.0, "75m SWBC" },
    {  4750.0,  5060.0, "60m SWBC" },
    {  5250.0,  5450.0, "60m Amateur" },
    {  5900.0,  6200.0, "49m SWBC" },
    {  7000.0,  7300.0, "40m Amateur" },
    {  7200.0,  7450.0, "41m SWBC" },
    {  9400.0,  9900.0, "31m SWBC" },
    { 10100.0, 10150.0, "30m Amateur" },
    { 11600.0, 12100.0, "25m SWBC" },
    { 13570.0, 13870.0, "22m SWBC" },
    { 14000.0, 14350.0, "20m Amateur" },
    { 15100.0, 15830.0, "19m SWBC" },
    { 17480.0, 17900.0, "16m SWBC" },
    { 18068.0, 18168.0, "17m Amateur" },
    { 21000.0, 21450.0, "15m Amateur" },
    { 21450.0, 21850.0, "13m SWBC" },
    { 24890.0, 24990.0, "12m Amateur" },
    { 25670.0, 26100.0, "11m SWBC" },
    { 26965.0, 27405.0, "CB" },
    { 28000.0, 29700.0, "10m Amateur" },
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
