#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sqlite3.h>

#include "osd/util/terrain_elevation.h"

#define TEST_DB_PATH "/tmp/msposd-terrain-elevation-test.db"
#define TEST_NODATA INT16_MIN

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
 * @param tile_x XYZ tile column.
 * @param tile_y XYZ tile row.
 * @param values Four row-major signed elevation samples.
 */
static void insert_tile(sqlite3 *db, int tile_x, int tile_y, const int16_t values[4])
{
	uint8_t blob[8];
	for (int i = 0; i < 4; i++) {
		uint16_t encoded = (uint16_t)values[i];
		blob[i * 2] = encoded & 0xff;
		blob[i * 2 + 1] = encoded >> 8;
	}

	sqlite3_stmt *statement = NULL;
	if (sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO elevation VALUES(1,?,?,2,2,0,0,?)", -1,
			&statement, NULL) != SQLITE_OK)
		exit(1);
	sqlite3_bind_int(statement, 1, tile_x);
	sqlite3_bind_int(statement, 2, tile_y);
	sqlite3_bind_blob(statement, 3, blob, sizeof(blob), SQLITE_TRANSIENT);
	if (sqlite3_step(statement) != SQLITE_DONE)
		exit(1);
	sqlite3_finalize(statement);
}

/**
 * Convert a global sample row to its Web Mercator latitude.
 *
 * @param global_y Global sample row, including fractional positions.
 * @return Latitude in degrees at the sample position.
 */
static double latitude_for_sample(double global_y)
{
	double tile_y = (global_y + 0.5) / 2.0;
	return atan(sinh(M_PI * (1.0 - tile_y))) * 180.0 / M_PI;
}

/**
 * Require a successful lookup with an expected elevation.
 *
 * @param lat Latitude in degrees.
 * @param lon Longitude in degrees.
 * @param expected Expected elevation in metres.
 */
static void expect_elevation(double lat, double lon, double expected)
{
	double actual = 0.0;
	if (!terrain_elevation_at(lat, lon, &actual) || fabs(actual - expected) > 0.001) {
		fprintf(stderr, "lookup %.8f,%.8f: got %.3f, expected %.3f\n", lat, lon, actual,
			expected);
		exit(1);
	}
}

/**
 * Require a lookup to report unavailable terrain.
 *
 * @param lat Latitude in degrees.
 * @param lon Longitude in degrees.
 */
static void expect_missing(double lat, double lon)
{
	double actual = 12345.0;
	if (terrain_elevation_at(lat, lon, &actual)) {
		fprintf(stderr, "lookup %.8f,%.8f unexpectedly returned %.3f\n", lat, lon, actual);
		exit(1);
	}
	if (actual != 12345.0) {
		fprintf(stderr, "failed lookup changed its output to %.3f\n", actual);
		exit(1);
	}
}

/**
 * Require the terrain database availability state to match an expectation.
 *
 * @param expected Expected availability state.
 */
static void expect_available(bool expected)
{
	bool actual = terrain_elevation_available();
	if (actual != expected) {
		fprintf(stderr, "terrain availability was %d, expected %d\n", actual, expected);
		exit(1);
	}
}

/**
 * Replace the test path with bytes that are not a SQLite database.
 *
 * @param bytes Bytes to write.
 * @param size Number of bytes to write.
 */
static void write_invalid_database(const void *bytes, size_t size)
{
	FILE *file = fopen(TEST_DB_PATH, "wb");
	if (!file || fwrite(bytes, 1, size, file) != size || fclose(file) != 0)
		exit(1);
}

/**
 * Create a valid SQLite file without the required terrain schema.
 *
 * @return Open database handle.
 */
static sqlite3 *create_empty_database(void)
{
	sqlite3 *db = NULL;
	unlink(TEST_DB_PATH);
	if (sqlite3_open(TEST_DB_PATH, &db) != SQLITE_OK)
		exit(1);
	return db;
}

/**
 * Add the supported metadata and terrain table to a database.
 *
 * @param db Open SQLite database.
 */
static void create_terrain_schema(sqlite3 *db)
{
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
}

/**
 * Build a synthetic terrain database and exercise the public lookup API.
 *
 * @return Zero when every lookup behaves as expected.
 */
int main(void)
{
	unlink(TEST_DB_PATH);
	expect_available(false);
	expect_missing(0.0, 0.0);
	static const char corrupt[] = "this is not sqlite";
	write_invalid_database(corrupt, strlen(corrupt));
	expect_available(false);
	expect_missing(0.0, 0.0);

	sqlite3 *db = create_empty_database();
	execute_sql(db, "CREATE TABLE unrelated(value TEXT)");
	sqlite3_close(db);
	expect_available(false);
	expect_missing(0.0, 0.0);

	db = create_empty_database();
	create_terrain_schema(db);
	const int16_t northwest[4] = { 0, 10, 20, 30 };
	const int16_t northeast[4] = { 40, 50, 60, 70 };
	const int16_t southwest[4] = { 80, -5, TEST_NODATA, 110 };
	insert_tile(db, 0, 0, northwest);
	insert_tile(db, 1, 0, northeast);
	insert_tile(db, 0, 1, southwest);
	sqlite3_close(db);
	expect_available(true);

	expect_elevation(latitude_for_sample(0.0), -135.0, 0.0);
	expect_elevation(latitude_for_sample(0.5), -90.0, 15.0);
	expect_elevation(latitude_for_sample(0.5), 0.0, 35.0);
	expect_elevation(latitude_for_sample(2.0), -135.0, 80.0);
	expect_elevation(latitude_for_sample(2.0), -45.0, -5.0);
	expect_missing(latitude_for_sample(3.0), -135.0);
	expect_missing(latitude_for_sample(2.0), 45.0);
	expect_missing(NAN, 0.0);
	expect_missing(0.0, NAN);
	expect_missing(INFINITY, 0.0);
	expect_missing(90.0, 0.0);
	expect_missing(-90.0, 0.0);
	expect_missing(0.0, 181.0);
	expect_missing(0.0, -181.0);
	expect_missing(85.05112878, 180.0);
	expect_missing(-85.05112878, -180.0);
	if (terrain_elevation_at(0.0, 0.0, NULL))
		return 1;

	if (sqlite3_open(TEST_DB_PATH, &db) != SQLITE_OK)
		return 1;
	execute_sql(db, "UPDATE elevation SET data=x'00' WHERE tile_x=0 AND tile_y=0");
	sqlite3_close(db);
	expect_available(false);
	expect_missing(latitude_for_sample(0.0), -135.0);

	if (sqlite3_open(TEST_DB_PATH, &db) != SQLITE_OK)
		return 1;
	insert_tile(db, 0, 0, northwest);
	execute_sql(db, "UPDATE elevation SET width=0 WHERE tile_x=0 AND tile_y=0");
	sqlite3_close(db);
	expect_available(false);
	expect_missing(latitude_for_sample(0.0), -135.0);

	if (sqlite3_open(TEST_DB_PATH, &db) != SQLITE_OK)
		return 1;
	insert_tile(db, 0, 0, northwest);
	execute_sql(db, "UPDATE meta SET value='tms' WHERE name='tile_scheme'");
	sqlite3_close(db);
	expect_available(false);
	expect_missing(latitude_for_sample(0.0), -135.0);

	unlink(TEST_DB_PATH);
	expect_available(false);
	expect_missing(latitude_for_sample(0.0), -135.0);
	puts("terrain elevation tests passed");
	return 0;
}
