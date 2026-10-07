/* Small geographic helpers for the server picker's distance filter.
 *
 * Portable core (no Vita headers); unit-tested on the host. The picker uses
 * these to turn a receiver's directory "gps" string into a lat/lon, to turn a
 * user-entered Maidenhead grid or "lat,lon" into a home reference point, and to
 * measure great-circle distance between the two.
 */
#ifndef VITASDR_GEO_H
#define VITASDR_GEO_H

/* Great-circle distance in kilometres (haversine, spherical earth). */
double geo_haversine_km(double lat1, double lon1, double lat2, double lon2);

/* Parse a KiwiSDR directory gps field, e.g. "(50.85, -0.66)" or "50.85,-0.66".
 * Writes the centre lat/lon and returns 1 on success, 0 if it can't parse two
 * in-range numbers. */
int geo_parse_gps(const char *s, double *lat, double *lon);

/* Parse a Maidenhead grid locator (4 or 6 chars, e.g. "IO90" or "IO90QU") to
 * the lat/lon of the square's centre. Returns 1 on success, 0 if malformed. */
int geo_maidenhead_to_latlon(const char *grid, double *lat, double *lon);

#endif /* VITASDR_GEO_H */
