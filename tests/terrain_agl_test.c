#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <sqlite3.h>

#include "osd/util/terrain_agl.h"

#define TEST_DB_PATH "/tmp/msposd-terrain-agl-test.db"

/**
 * Execute one SQL statement or terminate the test.
 *
 * @param db Open SQLite database.
 * @param sql SQL statement to execute.
 */
static void execute_sql(sqlite3 *db, const char *sql)
{
	char *error = NULL;
	if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
		fprintf(stderr, "SQL failed: %s\n", error ? error : "unknown error");
		sqlite3_free(error);
		exit(1);
	}
}

/**
 * Insert one 2x2 little-endian elevation tile.
 *
 * @param db Open SQLite database.
 * @param values Four row-major signed elevation samples.
 */
static void insert_tile(sqlite3 *db, const int16_t values[4])
{
	uint8_t blob[8];
	for (int i = 0; i < 4; i++) {
		uint16_t encoded = (uint16_t)values[i];
		blob[i * 2] = encoded & 0xff;
		blob[i * 2 + 1] = encoded >> 8;
	}

	sqlite3_stmt *statement = NULL;
	if (sqlite3_prepare_v2(db, "INSERT INTO elevation VALUES(1,0,0,2,2,80,120,?)", -1,
			&statement, NULL) != SQLITE_OK)
		exit(1);
	sqlite3_bind_blob(statement, 1, blob, sizeof(blob), SQLITE_TRANSIENT);
	if (sqlite3_step(statement) != SQLITE_DONE)
		exit(1);
	sqlite3_finalize(statement);
}

/** Create the synthetic terrain database used by AGL tests. */
static void create_database(void)
{
	unlink(TEST_DB_PATH);
	sqlite3 *db = NULL;
	if (sqlite3_open(TEST_DB_PATH, &db) != SQLITE_OK)
		exit(1);
	execute_sql(db, "CREATE TABLE meta(name TEXT PRIMARY KEY,value TEXT)");
	execute_sql(db,
		"INSERT INTO meta VALUES"
		"('zoom','1'),('encoding','int16_le'),('compression','none'),"
		"('tile_scheme','xyz'),('row_order','north_to_south'),"
		"('nodata','-32768'),('units','m')");
	execute_sql(db,
		"CREATE TABLE elevation(zoom INTEGER,tile_x INTEGER,tile_y INTEGER,width INTEGER,"
		"height INTEGER,min_m INTEGER,max_m INTEGER,data BLOB,"
		"PRIMARY KEY(zoom,tile_x,tile_y))");
	const int16_t values[4] = { 100, 120, 80, 90 };
	insert_tile(db, values);
	sqlite3_close(db);
}

/**
 * Convert a global sample column to its Web Mercator longitude.
 *
 * @param global_x Global sample column.
 * @return Longitude in degrees at the sample position.
 */
static double longitude_for_sample(double global_x)
{
	return (global_x + 0.5) / 4.0 * 360.0 - 180.0;
}

/**
 * Convert a global sample row to its Web Mercator latitude.
 *
 * @param global_y Global sample row.
 * @return Latitude in degrees at the sample position.
 */
static double latitude_for_sample(double global_y)
{
	double tile_y = (global_y + 0.5) / 2.0;
	return atan(sinh(M_PI * (1.0 - tile_y))) * 180.0 / M_PI;
}

/**
 * Require the current AGL to match an expected value.
 *
 * @param expected Expected AGL in metres.
 * @param now_ms Current monotonic timestamp.
 */
static void expect_agl(double expected, uint64_t now_ms)
{
	double actual = 0.0;
	if (!terrain_agl_get(&actual, now_ms) || fabs(actual - expected) > 0.001) {
		fprintf(stderr, "AGL got %.3f, expected %.3f at %llu ms\n", actual, expected,
			(unsigned long long)now_ms);
		exit(1);
	}
}

/**
 * Require the current AGL to be unavailable without changing output.
 *
 * @param now_ms Current monotonic timestamp.
 */
static void expect_unavailable(uint64_t now_ms)
{
	double actual = 12345.0;
	if (terrain_agl_get(&actual, now_ms) || actual != 12345.0) {
		fprintf(stderr, "AGL unexpectedly available or changed output at %llu ms\n",
			(unsigned long long)now_ms);
		exit(1);
	}
}

/**
 * Calibrate at the northwest sample after a witnessed disarmed state.
 *
 * @param altitude_m GPS altitude at home in metres.
 * @param start_ms Initial monotonic timestamp.
 */
static void calibrate_at_home(double altitude_m, uint64_t start_ms)
{
	terrain_agl_reset();
	terrain_agl_update_armed(false, start_ms);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), altitude_m, true,
		start_ms + 100);
	expect_agl(altitude_m - 100.0, start_ms + 100);
	terrain_agl_update_armed(true, start_ms + 200);
}

/** Exercise calibration, terrain changes and failure-state behavior. */
static void test_normal_flight(void)
{
	calibrate_at_home(500.0, 1000);
	expect_agl(0.0, 1200);

	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 550.0, true, 1300);
	expect_agl(30.0, 1300);
	terrain_agl_update_gps(latitude_for_sample(1), longitude_for_sample(0), 550.0, true, 1400);
	expect_agl(70.0, 1400);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 510.0, true, 1500);
	expect_agl(-10.0, 1500);

	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 510.0, false, 1600);
	expect_unavailable(1600);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 520.0, true, 1700);
	expect_agl(0.0, 1700);
	expect_unavailable(5001);
}

/** Exercise arming order, source offset cancellation and recalibration. */
static void test_calibration_lifecycle(void)
{
	terrain_agl_reset();
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), 500.0, true, 100);
	terrain_agl_update_armed(true, 200);
	expect_agl(400.0, 200);

	terrain_agl_reset();
	terrain_agl_update_armed(false, 1000);
	terrain_agl_update_armed(true, 1100);
	expect_unavailable(1100);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), 1500.0, true, 1200);
	expect_agl(0.0, 1200);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 1550.0, true, 1300);
	expect_agl(30.0, 1300);

	terrain_agl_update_armed(false, 1400);
	expect_agl(30.0, 1400);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), 700.0, true, 1500);
	expect_agl(-800.0, 1500);
	terrain_agl_update_armed(true, 1600);
	expect_agl(0.0, 1600);

	terrain_agl_update_armed(false, 1700);
	terrain_agl_update_armed(true, 5000);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 750.0, true, 7101);
	expect_agl(30.0, 7101);
}

/** Exercise missing terrain, late GPS and invalid numeric inputs. */
static void test_invalid_inputs(void)
{
	terrain_agl_reset();
	expect_unavailable(0);
	if (terrain_agl_get(NULL, 0))
		exit(1);

	terrain_agl_update_armed(false, 1000);
	terrain_agl_update_armed(true, 1100);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), 500.0, true, 3201);
	expect_agl(400.0, 3201);

	terrain_agl_reset();
	terrain_agl_update_armed(false, 4000);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(2), 500.0, true, 4100);
	terrain_agl_update_armed(true, 4200);
	expect_unavailable(4200);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), 500.0, true, 4300);
	expect_agl(400.0, 4300);

	calibrate_at_home(500.0, 5000);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(2), 550.0, true, 5300);
	expect_unavailable(5300);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(1), 550.0, true, 5400);
	expect_agl(30.0, 5400);
	terrain_agl_update_gps(NAN, 0.0, 0.0, true, 5500);
	expect_unavailable(5500);
}

/** Run the AGL state-machine test suite. */
int main(void)
{
	if (AGL_enabled) {
		fprintf(stderr, "AGL must be disabled by default\n");
		return 1;
	}
	AGL_enabled = true;
	create_database();
	test_normal_flight();
	test_calibration_lifecycle();
	test_invalid_inputs();

	unlink(TEST_DB_PATH);
	terrain_agl_reset();
	terrain_agl_update_armed(false, 6000);
	terrain_agl_update_gps(latitude_for_sample(0), longitude_for_sample(0), 500.0, true, 6100);
	terrain_agl_update_armed(true, 6200);
	expect_unavailable(6200);

	puts("terrain AGL tests passed");
	return 0;
}
