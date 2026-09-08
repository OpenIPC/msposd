#ifndef TERRAIN_AGL_H
#define TERRAIN_AGL_H

#include <stdbool.h>
#include <stdint.h>

/** Runtime availability set after validating the ground terrain database. */
extern bool AGL_enabled;

/** Reset all AGL calibration and current-value state. */
void terrain_agl_reset(void);

/**
 * Update the aircraft armed state.
 *
 * @param is_armed Current aircraft armed state.
 * @param now_ms Monotonic timestamp in milliseconds.
 */
void terrain_agl_update_armed(bool is_armed, uint64_t now_ms);

/**
 * Process a synchronized raw GPS position and altitude update.
 *
 * @param lat Latitude in degrees.
 * @param lon Longitude in degrees.
 * @param gps_altitude_m GPS altitude in metres.
 * @param fix_valid Whether the GPS fix is valid.
 * @param now_ms Monotonic timestamp in milliseconds.
 */
void terrain_agl_update_gps(double lat, double lon, double gps_altitude_m, bool fix_valid,
	uint64_t now_ms);

/**
 * Return the current height above terrain.
 *
 * @param agl_m Receives AGL in metres and remains unchanged on failure.
 * @param now_ms Monotonic timestamp in milliseconds.
 * @return true when the current AGL is valid; otherwise false.
 */
bool terrain_agl_get(double *agl_m, uint64_t now_ms);

#endif /* TERRAIN_AGL_H */
