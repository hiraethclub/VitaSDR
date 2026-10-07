/* See geo.h. */
#include "geo.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GEO_PI 3.14159265358979323846

static double deg2rad(double d) { return d * GEO_PI / 180.0; }

double geo_haversine_km(double lat1, double lon1, double lat2, double lon2)
{
    const double R = 6371.0;   /* mean earth radius, km */
    double dlat = deg2rad(lat2 - lat1);
    double dlon = deg2rad(lon2 - lon1);
    double s1 = sin(dlat / 2.0);
    double s2 = sin(dlon / 2.0);
    double a = s1 * s1 + cos(deg2rad(lat1)) * cos(deg2rad(lat2)) * s2 * s2;
    if (a > 1.0) a = 1.0;      /* guard against tiny FP overshoot */
    if (a < 0.0) a = 0.0;
    return 2.0 * R * asin(sqrt(a));
}

int geo_parse_gps(const char *s, double *lat, double *lon)
{
    if (!s) return 0;
    while (*s == ' ' || *s == '(') s++;
    char *end;
    double a = strtod(s, &end);
    if (end == s) return 0;
    s = end;
    while (*s == ' ' || *s == ',') s++;
    double b = strtod(s, &end);
    if (end == s) return 0;
    if (a < -90.0 || a > 90.0 || b < -180.0 || b > 180.0) return 0;
    if (lat) *lat = a;
    if (lon) *lon = b;
    return 1;
}

static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

int geo_maidenhead_to_latlon(const char *grid, double *lat, double *lon)
{
    if (!grid) return 0;
    size_t len = strlen(grid);
    if (len < 4) return 0;

    char f0 = up(grid[0]), f1 = up(grid[1]);
    if (f0 < 'A' || f0 > 'R' || f1 < 'A' || f1 > 'R') return 0;
    if (grid[2] < '0' || grid[2] > '9' || grid[3] < '0' || grid[3] > '9') return 0;

    double lonv = (f0 - 'A') * 20.0 - 180.0 + (grid[2] - '0') * 2.0;
    double latv = (f1 - 'A') * 10.0 -  90.0 + (grid[3] - '0') * 1.0;

    if (len >= 6) {
        char s0 = up(grid[4]), s1 = up(grid[5]);
        if (s0 < 'A' || s0 > 'X' || s1 < 'A' || s1 > 'X') return 0;
        lonv += (s0 - 'A') * (2.0 / 24.0) + (1.0 / 24.0);   /* + half subsquare */
        latv += (s1 - 'A') * (1.0 / 24.0) + (0.5 / 24.0);
    } else {
        lonv += 1.0;   /* + half square (2 deg wide) */
        latv += 0.5;   /* + half square (1 deg tall) */
    }

    if (lat) *lat = latv;
    if (lon) *lon = lonv;
    return 1;
}
