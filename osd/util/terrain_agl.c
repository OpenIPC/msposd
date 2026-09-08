/* Ground-station AGL calculation using raw GPS and offline terrain. */

#include "terrain_agl.h"

#include <math.h>
#include <string.h>

#include "terrain_elevation.h"

#define TERRAIN_AGL_GPS_MAX_AGE_MS 3000U
#define TERRAIN_AGL_ARM_CAPTURE_WINDOW_MS 2000U

typedef struct {
	bool seen_disarmed;
	bool armed;
	bool calibration_pending;
	bool calibrated;
	bool gps_valid;
	bool agl_valid;
	double last_lat;
	double last_lon;
	double last_gps_altitude_m;
	double altitude_offset_m;
	double current_agl_m;
	uint64_t arm_ms;
	uint64_t last_gps_ms;
} terrain_agl_state_t;

bool AGL_enabled = false;
static terrain_agl_state_t terrain_agl;

/**
 * Check whether a timestamp is recent without unsigned underflow.
 *
 * @param timestamp_ms Timestamp being checked.
 * @param now_ms Current monotonic timestamp.
 * @param max_age_ms Maximum permitted age in milliseconds.
 * @return true when the timestamp is not in the future or stale.
 */
static bool terrain_agl_timestamp_fresh(uint64_t timestamp_ms, uint64_t now_ms,
	uint64_t max_age_ms)
{
	return now_ms >= timestamp_ms && now_ms - timestamp_ms <= max_age_ms;
}

/**
 * Attempt calibration from the latest valid GPS sample.
 *
 * @param now_ms Current monotonic timestamp.
 * @return true when a new offset was successfully calibrated; otherwise false.
 */
static bool terrain_agl_try_calibrate(uint64_t now_ms)
{
	if (!terrain_agl.armed || !terrain_agl.calibration_pending || !terrain_agl.gps_valid)
		return false;
	if (!terrain_agl_timestamp_fresh(terrain_agl.arm_ms, now_ms,
			TERRAIN_AGL_ARM_CAPTURE_WINDOW_MS)) {
		terrain_agl.calibration_pending = false;
		return false;
	}
	if (!terrain_agl_timestamp_fresh(terrain_agl.last_gps_ms, now_ms,
			TERRAIN_AGL_GPS_MAX_AGE_MS))
		return false;

	double terrain_home_m;
	if (!terrain_elevation_at(terrain_agl.last_lat, terrain_agl.last_lon, &terrain_home_m)) {
		terrain_agl.calibration_pending = false;
		return false;
	}

	double offset = terrain_agl.last_gps_altitude_m - terrain_home_m;
	if (!isfinite(offset)) {
		terrain_agl.calibration_pending = false;
		return false;
	}

	terrain_agl.altitude_offset_m = offset;
	terrain_agl.current_agl_m = 0.0;
	terrain_agl.calibration_pending = false;
	terrain_agl.calibrated = true;
	terrain_agl.agl_valid = true;
	return true;
}

/** Reset all AGL calibration and current-value state. */
void terrain_agl_reset(void)
{
	memset(&terrain_agl, 0, sizeof(terrain_agl));
}

/**
 * Update the aircraft armed state.
 *
 * @param is_armed Current aircraft armed state.
 * @param now_ms Monotonic timestamp in milliseconds.
 */
void terrain_agl_update_armed(bool is_armed, uint64_t now_ms)
{
	if (!is_armed) {
		terrain_agl.seen_disarmed = true;
		terrain_agl.armed = false;
		terrain_agl.calibration_pending = false;
		return;
	}
	if (terrain_agl.armed)
		return;

	terrain_agl.armed = true;
	terrain_agl.arm_ms = now_ms;
	terrain_agl.calibration_pending = terrain_agl.seen_disarmed;
	(void)terrain_agl_try_calibrate(now_ms);
}

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
	uint64_t now_ms)
{
	bool valid = fix_valid && isfinite(lat) && isfinite(lon) && isfinite(gps_altitude_m) &&
		lat >= -90.0 && lat <= 90.0 && lon >= -180.0 && lon <= 180.0;
	if (!valid) {
		terrain_agl.gps_valid = false;
		terrain_agl.agl_valid = false;
		return;
	}

	terrain_agl.last_lat = lat;
	terrain_agl.last_lon = lon;
	terrain_agl.last_gps_altitude_m = gps_altitude_m;
	terrain_agl.last_gps_ms = now_ms;
	terrain_agl.gps_valid = true;

	if (terrain_agl.calibration_pending && terrain_agl_try_calibrate(now_ms))
		return;

	double terrain_current_m;
	if (!terrain_elevation_at(lat, lon, &terrain_current_m)) {
		terrain_agl.agl_valid = false;
		return;
	}

	double agl = gps_altitude_m - terrain_current_m;
	if (terrain_agl.calibrated)
		agl -= terrain_agl.altitude_offset_m;
	if (!isfinite(agl)) {
		terrain_agl.agl_valid = false;
		return;
	}
	terrain_agl.current_agl_m = agl;
	terrain_agl.agl_valid = true;
}

/**
 * Return the current height above terrain.
 *
 * @param agl_m Receives AGL in metres and remains unchanged on failure.
 * @param now_ms Monotonic timestamp in milliseconds.
 * @return true when the current AGL is valid; otherwise false.
 */
bool terrain_agl_get(double *agl_m, uint64_t now_ms)
{
	if (!agl_m || !terrain_agl.gps_valid || !terrain_agl.agl_valid ||
		!terrain_agl_timestamp_fresh(terrain_agl.last_gps_ms, now_ms,
			TERRAIN_AGL_GPS_MAX_AGE_MS) ||
		!isfinite(terrain_agl.current_agl_m))
		return false;

	*agl_m = terrain_agl.current_agl_m;
	return true;
}
