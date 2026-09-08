/* Ground-station terrain lookup backed by gs/maps/elevation.db. */

#include "terrain_elevation.h"

#include <math.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "simple_ini.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TERRAIN_MAX_MERCATOR_LAT 85.05112878
#define TERRAIN_NODATA INT16_MIN

/**
 * Resolve the terrain database used by the ground station.
 *
 * @param path Destination buffer.
 * @param path_size Size of the destination buffer in bytes.
 */
static void terrain_database_path(char *path, size_t path_size)
{
#ifdef TERRAIN_ELEVATION_DB_PATH
	snprintf(path, path_size, "%s", TERRAIN_ELEVATION_DB_PATH);
#else
	snprintf(path, path_size, "%s/gs/maps/elevation.db", exe_dir());
#endif
}

/**
 * Validate the terrain schema and obtain its grid geometry.
 *
 * @param db Open read-only SQLite database.
 * @param zoom Receives the Web Mercator zoom level.
 * @param tile_size Receives the square tile width in samples.
 * @return true when the schema is supported; otherwise false.
 */
static bool terrain_read_schema(sqlite3 *db, int *zoom, int *tile_size)
{
	static const char sql[] =
		"SELECT "
		"(SELECT value FROM meta WHERE name='zoom'),"
		"(SELECT value FROM meta WHERE name='encoding'),"
		"(SELECT value FROM meta WHERE name='compression'),"
		"(SELECT value FROM meta WHERE name='tile_scheme'),"
		"(SELECT value FROM meta WHERE name='row_order'),"
		"(SELECT value FROM meta WHERE name='nodata'),"
		"(SELECT value FROM meta WHERE name='units')";
	sqlite3_stmt *statement = NULL;
	bool valid = false;

	if (sqlite3_prepare_v2(db, sql, -1, &statement, NULL) != SQLITE_OK)
		return false;

	if (sqlite3_step(statement) == SQLITE_ROW) {
		const char *zoom_text = (const char *)sqlite3_column_text(statement, 0);
		const char *encoding = (const char *)sqlite3_column_text(statement, 1);
		const char *compression = (const char *)sqlite3_column_text(statement, 2);
		const char *scheme = (const char *)sqlite3_column_text(statement, 3);
		const char *row_order = (const char *)sqlite3_column_text(statement, 4);
		const char *nodata = (const char *)sqlite3_column_text(statement, 5);
		const char *units = (const char *)sqlite3_column_text(statement, 6);
		char *end = NULL;
		long parsed_zoom = zoom_text ? strtol(zoom_text, &end, 10) : -1;

		if (zoom_text && end && *end == '\0' && parsed_zoom >= 0 && parsed_zoom <= 22 &&
			encoding && strcmp(encoding, "int16_le") == 0 &&
			compression && strcmp(compression, "none") == 0 &&
			scheme && strcmp(scheme, "xyz") == 0 &&
			row_order && strcmp(row_order, "north_to_south") == 0 &&
			nodata && strcmp(nodata, "-32768") == 0 && units && strcmp(units, "m") == 0) {
			*zoom = (int)parsed_zoom;
			valid = true;
		}
	}
	sqlite3_finalize(statement);
	if (!valid)
		return false;

	valid = false;
	if (sqlite3_prepare_v2(db,
			"SELECT width, height, data FROM elevation WHERE zoom=? "
			"ORDER BY tile_x, tile_y LIMIT 1",
			-1, &statement, NULL) != SQLITE_OK)
		return false;
	if (sqlite3_bind_int(statement, 1, *zoom) != SQLITE_OK) {
		sqlite3_finalize(statement);
		return false;
	}
	if (sqlite3_step(statement) == SQLITE_ROW) {
		sqlite3_int64 width = sqlite3_column_int64(statement, 0);
		sqlite3_int64 height = sqlite3_column_int64(statement, 1);
		const void *blob = sqlite3_column_blob(statement, 2);
		int blob_size = sqlite3_column_bytes(statement, 2);
		if (sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
			sqlite3_column_type(statement, 1) == SQLITE_INTEGER &&
			sqlite3_column_type(statement, 2) == SQLITE_BLOB && width > 0 && width <= 4096 &&
			width == height && blob && blob_size >= 0 &&
			(sqlite3_int64)blob_size == width * height * (sqlite3_int64)sizeof(int16_t)) {
			*tile_size = (int)width;
			valid = true;
		}
	}
	sqlite3_finalize(statement);
	return valid;
}

/**
 * Check whether a supported terrain database containing tiles is available.
 *
 * @return true when the ground terrain database can be used; otherwise false.
 */
bool terrain_elevation_available(void)
{
	char path[1024];
	sqlite3 *db = NULL;
	int zoom = 0;
	int tile_size = 0;
	bool available = false;

	terrain_database_path(path, sizeof(path));
	if (!path[0] || strlen(path) >= sizeof(path) - 1)
		return false;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		goto done;
	sqlite3_busy_timeout(db, 250);
	available = terrain_read_schema(db, &zoom, &tile_size);

done:
	if (db)
		sqlite3_close(db);
	return available;
}

/**
 * Read one signed elevation sample from the global pixel grid.
 *
 * @param statement Prepared elevation-tile query.
 * @param zoom Terrain zoom level.
 * @param tile_size Tile width and height in samples.
 * @param global_x Global sample column.
 * @param global_y Global sample row.
 * @param value Receives the sample in metres.
 * @return true when the sample exists and is valid; otherwise false.
 */
static bool terrain_read_sample(sqlite3_stmt *statement, int zoom, int tile_size,
	int64_t global_x, int64_t global_y, double *value)
{
	int64_t tile_x = global_x / tile_size;
	int64_t tile_y = global_y / tile_size;
	int64_t pixel_x = global_x % tile_size;
	int64_t pixel_y = global_y % tile_size;
	bool found = false;

	if (pixel_x < 0) {
		pixel_x += tile_size;
		tile_x--;
	}
	if (pixel_y < 0) {
		pixel_y += tile_size;
		tile_y--;
	}

	sqlite3_reset(statement);
	sqlite3_clear_bindings(statement);
	if (sqlite3_bind_int(statement, 1, zoom) != SQLITE_OK ||
		sqlite3_bind_int64(statement, 2, tile_x) != SQLITE_OK ||
		sqlite3_bind_int64(statement, 3, tile_y) != SQLITE_OK)
		return false;
	if (sqlite3_step(statement) == SQLITE_ROW) {
		sqlite3_int64 width = sqlite3_column_int64(statement, 0);
		sqlite3_int64 height = sqlite3_column_int64(statement, 1);
		const uint8_t *blob = sqlite3_column_blob(statement, 2);
		int blob_size = sqlite3_column_bytes(statement, 2);
		size_t expected_size = (size_t)tile_size * (size_t)tile_size * sizeof(int16_t);
		size_t offset = ((size_t)pixel_y * (size_t)tile_size + (size_t)pixel_x) * 2U;

		if (sqlite3_column_type(statement, 0) == SQLITE_INTEGER &&
			sqlite3_column_type(statement, 1) == SQLITE_INTEGER &&
			sqlite3_column_type(statement, 2) == SQLITE_BLOB && width == tile_size &&
			height == tile_size && blob && blob_size >= 0 && (size_t)blob_size == expected_size &&
			offset <= expected_size - sizeof(int16_t)) {
			uint16_t encoded = (uint16_t)blob[offset] | ((uint16_t)blob[offset + 1] << 8);
			int16_t sample = (int16_t)encoded;
			if (sample != TERRAIN_NODATA) {
				*value = sample;
				found = true;
			}
		}
	}
	sqlite3_reset(statement);
	return found;
}

/**
 * Look up terrain elevation at a geographic coordinate.
 *
 * @param lat Latitude in degrees.
 * @param lon Longitude in degrees.
 * @param elevation_m Receives elevation above mean sea level in metres.
 * @return true when elevation data exists; otherwise false.
 */
bool terrain_elevation_at(double lat, double lon, double *elevation_m)
{
	char path[1024];
	sqlite3 *db = NULL;
	sqlite3_stmt *statement = NULL;
	int zoom = 0;
	int tile_size = 0;
	bool found = false;

	if (!elevation_m || !isfinite(lat) || !isfinite(lon) ||
		lat < -TERRAIN_MAX_MERCATOR_LAT || lat > TERRAIN_MAX_MERCATOR_LAT || lon < -180.0 ||
		lon > 180.0)
		return false;

	terrain_database_path(path, sizeof(path));
	if (!path[0] || strlen(path) >= sizeof(path) - 1)
		return false;
	if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		goto done;
	sqlite3_busy_timeout(db, 250);
	if (!terrain_read_schema(db, &zoom, &tile_size))
		goto done;

	if (sqlite3_prepare_v2(db,
			"SELECT width, height, data FROM elevation "
			"WHERE zoom=? AND tile_x=? AND tile_y=?",
			-1, &statement, NULL) != SQLITE_OK)
		goto done;

	double world_tiles = ldexp(1.0, zoom);
	double latitude_radians = lat * M_PI / 180.0;
	double tile_x = (lon + 180.0) / 360.0 * world_tiles;
	double tile_y =
		(1.0 - log(tan(latitude_radians) + 1.0 / cos(latitude_radians)) / M_PI) * 0.5 *
		world_tiles;
	double global_x = tile_x * tile_size - 0.5;
	double global_y = tile_y * tile_size - 0.5;
	if (!isfinite(global_x) || !isfinite(global_y) || global_x < (double)INT64_MIN ||
		global_x > (double)INT64_MAX - 1.0 || global_y < (double)INT64_MIN ||
		global_y > (double)INT64_MAX - 1.0)
		goto done;
	int64_t x0 = (int64_t)floor(global_x);
	int64_t y0 = (int64_t)floor(global_y);
	double dx = global_x - x0;
	double dy = global_y - y0;
	double v00, v10, v01, v11;

	if (!terrain_read_sample(statement, zoom, tile_size, x0, y0, &v00))
		goto done;
	if (!terrain_read_sample(statement, zoom, tile_size, x0 + 1, y0, &v10))
		v10 = v00;
	if (!terrain_read_sample(statement, zoom, tile_size, x0, y0 + 1, &v01))
		v01 = v00;
	if (!terrain_read_sample(statement, zoom, tile_size, x0 + 1, y0 + 1, &v11))
		v11 = v10;

	double top = v00 + (v10 - v00) * dx;
	double bottom = v01 + (v11 - v01) * dx;
	*elevation_m = top + (bottom - top) * dy;
	found = true;

done:
	if (statement)
		sqlite3_finalize(statement);
	if (db)
		sqlite3_close(db);
	return found;
}
