/*
 * sun - sunrise / sunset from position and date (the "sunrise equation",
 * NOAA simplification; about a minute of error, plenty for "before dark").
 */
#include <math.h>
#include "bridge.h"

#define RAD (M_PI / 180.0)

bool sun_times(double lat, double lon, time_t when, time_t *rise, time_t *set)
{
    double jd = when / 86400.0 + 2440587.5;
    double n = floor(jd - 2451545.0 + 0.0008 + 0.5);         /* day number since J2000 */
    double jstar = n - lon / 360.0;                          /* mean solar time, east positive */
    double m = fmod(357.5291 + 0.98560028 * jstar, 360.0);
    double c = 1.9148 * sin(m * RAD) + 0.02 * sin(2 * m * RAD) + 0.0003 * sin(3 * m * RAD);
    double lambda = fmod(m + c + 180.0 + 102.9372, 360.0);
    double transit = 2451545.0 + jstar + 0.0053 * sin(m * RAD) - 0.0069 * sin(2 * lambda * RAD);
    double sin_d = sin(lambda * RAD) * sin(23.4397 * RAD);
    double cos_d = cos(asin(sin_d));
    double cos_w = (sin(-0.833 * RAD) - sin(lat * RAD) * sin_d) / (cos(lat * RAD) * cos_d);
    if (cos_w < -1 || cos_w > 1) {
        return false;                                        /* polar day / night */
    }
    double w = acos(cos_w) / RAD;
    *rise = (time_t)((transit - w / 360.0 - 2440587.5) * 86400.0);
    *set = (time_t)((transit + w / 360.0 - 2440587.5) * 86400.0);
    return true;
}
