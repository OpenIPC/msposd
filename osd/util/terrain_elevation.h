#ifndef TERRAIN_ELEVATION_H
#define TERRAIN_ELEVATION_H

#include <stdbool.h>

/**
 * Check whether a supported terrain database containing tiles is available.
 *
 * @return true when the ground terrain database can be used; otherwise false.
 */
bool terrain_elevation_available(void);

/**
 * Look up terrain elevation at a geographic coordinate.
 *
 * @param lat Latitude in degrees.
 * @param lon Longitude in degrees.
 * @param elevation_m Receives elevation above mean sea level in metres. Must
 * point to writable storage; its value is unchanged on failure.
 * @return true when elevation data exists; false for invalid arguments,
 * unavailable data, unsupported schemas, malformed tiles, or SQLite errors.
 */
bool terrain_elevation_at(double lat, double lon, double *elevation_m);

#endif /* TERRAIN_ELEVATION_H */
