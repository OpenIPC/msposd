/*
 * poi_osd.c — draw nearby map landmarks (POIs) as direction markers on the OSD.
 *
 * Ground-Side rendering only (_x86 / __ROCKCHIP__): this file is #included from
 * osd.c inside that guard, so the camera/OpenIPC builds never see it or sqlite3.
 *
 * POIs of kind='place' are read from the same landmarks.db the gs/ map uses, via
 * a bounding-box query around the plane that is refreshed only when the plane has
 * moved ~1/4 of the range. The small candidate set is then projected each frame:
 *   - horizontal: azimuth offset from heading -> x via tangent/FOV mapping
 *   - vertical:   above perspective_alt_m, the depression angle -> y using the
 *                 AHI horizon model (same f/pitch/roll), so a POI sits over its
 *                 spot in the video; below it, distance from the AHI centre down
 * See documentation/poi-osd-direction-plan.md.
 */

#include <sqlite3.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define POI_MAX        256          /* max candidates kept in memory */
#define POI_EARTH_R    6371000.0    /* mean earth radius, metres */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define POI_D2R (M_PI / 180.0)
#define POI_PERSP_HYST 10.0         /* m, hysteresis around perspective_alt_m */

typedef struct {
	char   label[64];
	double lat, lon;                /* degrees */
	double ele;                     /* terrain elevation MSL, valid if ele_state > 0 */
	int8_t ele_state;               /* 0 = not looked up, 1 = known, -1 = unavailable */
} poi_t;

/* Per-frame projection inputs shared by POIs and targets. */
typedef struct {
	bool   persp;                   /* perspective (true) or distance-based Y */
	double lat, lon;                /* plane position, degrees */
	double heading, pitch_deg, roll_deg;
	int    pos_y;                   /* AHI centre (includes Ctrl+Up/Down tilt) */
	double f, f_h, cx;              /* vertical/horizontal focal length, centre x */
	bool   have_msl;                /* plane_msl valid (terrain data present) */
	double plane_msl;               /* plane altitude on the terrain datum, m */
	double h_home;                  /* plane height above home, m */
} poi_view_t;

static int   poi_enabled   = -1;    /* -1 = config not read yet */
static char  poi_db_path[256] = "gs/maps/landmarks.db";
static int   poi_range_m   = 10000;
static int   poi_fov_deg   = 45;    /* half-angle filter around heading */
static int   poi_font_size = 14;    /* label font size (30% smaller than the old 20) */
static int   poi_persp_alt_m = 100; /* perspective Y above this height, distance Y below */
static int   poi_heading_tc_ms = 20; /* heading low-pass time constant, ms; 0 = off  ; 300 spread over ~6 frames — smooth but 300ms latency */

static poi_t poi_list[POI_MAX];
static int   poi_count     = 0;
static bool  poi_have_load = false;
static double poi_load_lat = 0, poi_load_lon = 0;   /* bbox centre of last load */
static bool  poi_home_set  = false;
static double poi_home_alt = 0;     /* GPS altitude latched at arming, m */

static int poi_db_has_pois(void) {
	sqlite3 *db = NULL;
	sqlite3_stmt *st = NULL;
	int has_pois = 0;

	if (sqlite3_open_v2(poi_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		if (db) sqlite3_close(db);
		return 0;
	}

	if (sqlite3_prepare_v2(db, "SELECT 1 FROM landmarks LIMIT 1", -1, &st, NULL) == SQLITE_OK)
		has_pois = sqlite3_step(st) == SQLITE_ROW;

	sqlite3_finalize(st);
	sqlite3_close(db);
	return has_pois;
}

/* Read [poi] config once. Section is fixed (not per-FC) so it is global. */
static void poi_read_config(void) {
	int v;
	int has_enabled_cfg = ReadIniInt("poi", "enabled", &v);
	int enabled_cfg = has_enabled_cfg ? v : 0;
	if (ReadIniInt("poi", "range_m", &v) && v > 0) poi_range_m = v;
	if (ReadIniInt("poi", "fov_deg", &v) && v > 0) poi_fov_deg = v;
	if (ReadIniInt("poi", "font_size", &v) && v > 0) poi_font_size = v;
	if (ReadIniInt("poi", "heading_tc_ms", &v)) poi_heading_tc_ms = v;  /* 0 disables */
	ReadIniString("poi", "db_path", poi_db_path, sizeof(poi_db_path));
	if (poi_db_path[0] && poi_db_path[0] != '/') {
		char abs[512];
		snprintf(abs, sizeof(abs), "%s/%s", exe_dir(), poi_db_path);
		snprintf(poi_db_path, sizeof(poi_db_path), "%s", abs);
	}
	if (ReadIniInt("poi", "perspective_alt_m", &v) && v > 0) poi_persp_alt_m = v;

	if (has_enabled_cfg)
		poi_enabled = enabled_cfg ? poi_db_has_pois() : 0;
	else
		poi_enabled = poi_db_has_pois();

	if (verbose)
		printf("[poi] %s (db: %s)\n", poi_enabled ? "enabled" : "disabled", poi_db_path);
}

static void poi_toggle_enabled(void) {
	if (poi_enabled < 0) poi_read_config();

	if (poi_enabled) {
		poi_enabled = 0;
		poi_count = 0;
		poi_have_load = false;
		printf("[poi] disabled\n");
		return;
	}

	if (!poi_db_has_pois()) {
		printf("[poi] cannot enable: no POIs in %s\n", poi_db_path);
		return;
	}

	poi_enabled = 1;
	poi_have_load = false;
	printf("[poi] enabled\n");
}

/* Load kind='place' rows within a lat/lon bbox of `range_m` around (lat,lon). */
static void poi_load_bbox(double lat, double lon) {
	poi_count = 0;
	poi_have_load = true;
	poi_load_lat = lat;
	poi_load_lon = lon;

	double dLat = poi_range_m / 111320.0;
	double cosl = cos(lat * POI_D2R);
	if (cosl < 0.01) cosl = 0.01;
	double dLon = poi_range_m / (111320.0 * cosl);

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(poi_db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
		fprintf(stderr, "[poi] cannot open %s: %s\n", poi_db_path, sqlite3_errmsg(db));
		if (db) sqlite3_close(db);
		poi_enabled = 0;            /* disable for the session */
		return;
	}

	/* Draw the kind/subtype pairs the user selected in the preflight tree
	 * (poi_selection table). Fall back to place-only when that table is missing
	 * or unseeded, so the OSD is never blank by surprise. A user who has a seeded
	 * selection and disables everything gets an empty draw — that is intentional. */
	int have_selection = 0;
	sqlite3_stmt *cnt = NULL;
	if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM poi_selection", -1, &cnt, NULL)
	        == SQLITE_OK && sqlite3_step(cnt) == SQLITE_ROW)
		have_selection = sqlite3_column_int(cnt, 0) > 0;
	sqlite3_finalize(cnt);

	const char *sql_sel =
		"SELECT l.name_en, l.lat, l.lon FROM landmarks l "
		"JOIN poi_selection s ON s.enabled=1 AND s.kind=l.kind "
		"AND s.subtype=COALESCE(NULLIF(l.subtype,''),'') "
		"WHERE l.name_en IS NOT NULL AND l.name_en <> '' "
		"AND l.lat BETWEEN ? AND ? AND l.lon BETWEEN ? AND ?";
	const char *sql_place =
		"SELECT name_en, lat, lon FROM landmarks "
		"WHERE kind='place' AND name_en IS NOT NULL AND name_en <> '' "
		"AND lat BETWEEN ? AND ? AND lon BETWEEN ? AND ?";
	const char *sql = have_selection ? sql_sel : sql_place;  /* same 4 bind params */
	sqlite3_stmt *st = NULL;
	if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) {
		sqlite3_bind_double(st, 1, lat - dLat);
		sqlite3_bind_double(st, 2, lat + dLat);
		sqlite3_bind_double(st, 3, lon - dLon);
		sqlite3_bind_double(st, 4, lon + dLon);
		while (poi_count < POI_MAX && sqlite3_step(st) == SQLITE_ROW) {
			const unsigned char *nm = sqlite3_column_text(st, 0);
			poi_t *p = &poi_list[poi_count];
			snprintf(p->label, sizeof(p->label), "%s", nm ? (const char *)nm : "?");
			p->lat = sqlite3_column_double(st, 1);
			p->lon = sqlite3_column_double(st, 2);
			p->ele_state = 0;
			poi_count++;
		}
	}
	sqlite3_finalize(st);
	sqlite3_close(db);
}

/* Great-circle distance (m) and initial bearing (deg) from (lat1,lon1)->(lat2,lon2). */
static void poi_dist_bearing(double lat1, double lon1, double lat2, double lon2,
                             double *dist_m, double *bearing_deg) {
	double p1 = lat1 * POI_D2R, p2 = lat2 * POI_D2R;
	double dphi = (lat2 - lat1) * POI_D2R, dlmb = (lon2 - lon1) * POI_D2R;
	double a = sin(dphi / 2) * sin(dphi / 2) +
	           cos(p1) * cos(p2) * sin(dlmb / 2) * sin(dlmb / 2);
	*dist_m = 2 * POI_EARTH_R * asin(fmin(1.0, sqrt(a)));
	double y = sin(dlmb) * cos(p2);
	double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dlmb);
	double b = atan2(y, x) / POI_D2R;
	*bearing_deg = (b < 0) ? b + 360.0 : b;
}

/* Normalize an angle (deg) to [-180, 180]. */
static double poi_norm180(double a) {
	while (a > 180.0)  a -= 360.0;
	while (a < -180.0) a += 360.0;
	return a;
}

/*
 * Light circular low-pass on heading. MSP_ATTITUDE heading has 1-degree
 * resolution, so turning steps the POIs ~17-34 px per degree tick; this smooths
 * the steps into sub-degree motion. Filtered as a unit vector (sin/cos) so the
 * 0/360 wrap is handled, with a dt-aware factor so the time constant holds
 * regardless of frame rate. Pass-through when poi_heading_tc_ms <= 0 — set that
 * once a decidegree-resolution heading source makes smoothing unnecessary.
 */
static double poi_filter_heading(double heading_deg) {
	if (poi_heading_tc_ms <= 0) return heading_deg;        /* filter off */

	static int      init = 0;
	static uint64_t last_ms = 0;
	static double   hf_sin = 0, hf_cos = 1;

	uint64_t now = get_time_ms();
	double r = heading_deg * POI_D2R, ms = sin(r), mc = cos(r);
	if (!init) {
		hf_sin = ms; hf_cos = mc; init = 1;
	} else {
		double dt = (now - last_ms) / 1000.0;
		double tc = poi_heading_tc_ms / 1000.0;
		double a  = dt / (tc + dt);                       /* dt-aware EMA factor */
		hf_sin += a * (ms - hf_sin);
		hf_cos += a * (mc - hf_cos);
	}
	last_ms = now;
	double h = atan2(hf_sin, hf_cos) / POI_D2R;
	return (h < 0) ? h + 360.0 : h;
}

/**
 * Latch the home GPS altitude on each disarmed->armed edge with a valid fix.
 * Starts as "armed" so a ground station joining mid-flight does not latch.
 * @param alt_m current GPS altitude, metres
 */
static void poi_update_home(int16_t alt_m) {
	static bool was_armed = true, pending = false;
	if (armed && !was_armed) pending = true;
	if (!armed) pending = false;
	was_armed = armed;
	if (pending && current_gps_valid()) {
		poi_home_alt = alt_m;
		poi_home_set = true;
		pending = false;
	}
}

/**
 * Plane height above a point. Uses the point's terrain elevation when terrain
 * data is available (looked up once, cached in *ele), else assumes the point
 * lies at home altitude.
 * @param v         frame view (plane altitudes)
 * @param lat, lon  point position, degrees
 * @param ele       cached terrain elevation of the point, metres MSL
 * @param ele_state cache state: 0 unknown, 1 known, -1 unavailable
 * @return height of the plane above the point, metres
 */
static double poi_rel_height(const poi_view_t *v, double lat, double lon,
                             double *ele, int8_t *ele_state) {
	if (!v->have_msl) return v->h_home;
	if (*ele_state == 0)
		*ele_state = terrain_elevation_at(lat, lon, ele) ? 1 : -1;
	return (*ele_state > 0) ? v->plane_msl - *ele : v->h_home;
}

/**
 * Project a geographic point to screen coordinates.
 * @param v           frame view
 * @param lat, lon    point position, degrees
 * @param ele         cached terrain elevation (see poi_rel_height)
 * @param ele_state   cache state (see poi_rel_height)
 * @param range_limit skip points beyond poi_range_m
 * @param x, y        receive screen position, pixels
 * @param dist        receives ground distance, metres
 * @return true when the point is within FOV/range and on screen
 */
static bool poi_project(const poi_view_t *v, double lat, double lon,
                        double *ele, int8_t *ele_state, bool range_limit,
                        double *x, double *y, double *dist) {
	double brg;
	poi_dist_bearing(v->lat, v->lon, lat, lon, dist, &brg);
	if (range_limit && *dist > poi_range_m) return false;

	double az = poi_norm180(brg - v->heading);
	if (fabs(az) > poi_fov_deg) return false;
	*x = v->cx + v->f_h * tan(az * POI_D2R);

	if (v->persp) {
		/* depression angle, mapped like a pitch-ladder line at k = el */
		double h  = poi_rel_height(v, lat, lon, ele, ele_state);
		double el = atan2(-h, *dist) / POI_D2R;
		double ang = el - v->pitch_deg;
		if (fabs(ang) >= 89.0) return false;
		*y = v->pos_y - v->f * tan(ang * POI_D2R);

		/* roll about the screen centre, as the ladder does (Render_gs.c) */
		double r = v->roll_deg * POI_D2R;
		double dx = *x - OVERLAY_WIDTH / 2.0, dy = *y - OVERLAY_HEIGHT / 2.0;
		*x = OVERLAY_WIDTH / 2.0 + dx * cos(r) - dy * sin(r);
		*y = OVERLAY_HEIGHT / 2.0 + dx * sin(r) + dy * cos(r);
	} else {
		/* distance: farthest at the AHI centre, closest near the bottom */
		double frac = fmin(*dist / (double)poi_range_m, 1.0);   /* 0 near .. 1 far */
		double maxY = OVERLAY_HEIGHT * 0.95;
		*y = v->pos_y + (1.0 - frac) * (maxY - v->pos_y);
	}
	return *x >= 0 && *x <= OVERLAY_WIDTH && *y >= 0 && *y <= OVERLAY_HEIGHT;
}

/*
 * Preflight targets — up to POI_TARGETS named points authored in the preflight
 * map and stored in the landmarks DB (`waypoints`, kinds 'target' and
 * 'target2'..'targetN'), so they travel with the POIs as part of the map pack.
 * Slot 0 keeps the historical kind='target' row, so a pack written by an older
 * preflight still shows its target here. Cached and refreshed at most once a
 * second. A legacy target in gs/state.ini [target] is honoured as a migration
 * fallback. This is the single reader; map_render.c consumes them via
 * poi_target_count()/poi_get_target_at().
 */
#define POI_TARGETS    5
#define POI_TARGET_NAME 32
typedef struct {
	double lat, lon;
	char   name[POI_TARGET_NAME];
	double ele;                     /* cached terrain elevation, see poi_t */
	int8_t ele_state;
} poi_target_t;
static poi_target_t poi_targets[POI_TARGETS];
static int      poi_ntarget = 0;                 /* how many slots are filled */
static uint64_t poi_target_last_ms = 0;
/* slot 0 mirrors, kept so the legacy ini fallback below stays readable */
static bool     poi_target_set = false;
static double   poi_target_lat = 0, poi_target_lon = 0;

/* Legacy fallback: read the target from gs/state.ini [target]. */
static bool poi_target_from_ini(void) {
	int set = 0;
	if (!ReadIniIntPath(GS_STATE_PATH, "target", "set", &set) || !set) return false;
	char buf[64];
	if (!ReadIniStringPath(GS_STATE_PATH, "target", "lat", buf, sizeof(buf))) return false;
	poi_target_lat = atof(buf);
	if (!ReadIniStringPath(GS_STATE_PATH, "target", "lon", buf, sizeof(buf))) return false;
	poi_target_lon = atof(buf);
	return true;
}

static void poi_refresh_target(void) {
	uint64_t now = get_time_ms();
	if (poi_target_last_ms != 0 && now - poi_target_last_ms < 1000) return;
	poi_target_last_ms = now;

	poi_target_t prev[POI_TARGETS];              /* to keep cached elevations */
	int nprev = poi_ntarget;
	memcpy(prev, poi_targets, sizeof(prev));
	poi_ntarget = 0;
	poi_target_set = false;

	sqlite3 *db = NULL;
	if (sqlite3_open_v2(poi_db_path, &db, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
		sqlite3_stmt *st = NULL;
		/* One pass over the slot rows; ordering by kind puts 'target' (slot 0)
		 * first, then 'target2'..'targetN' in numeric order for N < 10. */
		if (sqlite3_prepare_v2(db,
		        "SELECT lat, lon, name FROM waypoints "
		        "WHERE kind='target' OR kind LIKE 'target_' ORDER BY kind",
		        -1, &st, NULL) == SQLITE_OK) {
			while (poi_ntarget < POI_TARGETS && sqlite3_step(st) == SQLITE_ROW) {
				poi_target_t *t = &poi_targets[poi_ntarget++];
				t->lat = sqlite3_column_double(st, 0);
				t->lon = sqlite3_column_double(st, 1);
				const unsigned char *nm = sqlite3_column_text(st, 2);
				snprintf(t->name, sizeof(t->name), "%s", nm ? (const char *)nm : "");
			}
		}
		sqlite3_finalize(st);
	}
	if (db) sqlite3_close(db);

	if (poi_ntarget == 0 && poi_target_from_ini()) {   /* migration: legacy state.ini */
		poi_targets[0].lat = poi_target_lat;
		poi_targets[0].lon = poi_target_lon;
		poi_targets[0].name[0] = '\0';
		poi_ntarget = 1;
	}
	for (int i = 0; i < poi_ntarget; i++) {        /* reuse elevation if unmoved */
		poi_target_t *t = &poi_targets[i];
		bool same = i < nprev && prev[i].lat == t->lat && prev[i].lon == t->lon;
		t->ele       = same ? prev[i].ele : 0;
		t->ele_state = same ? prev[i].ele_state : 0;
	}
	poi_target_set = poi_ntarget > 0;
	if (poi_target_set) {
		poi_target_lat = poi_targets[0].lat;
		poi_target_lon = poi_targets[0].lon;
	}
}

/* Shared accessors so map_render.c need not open the DB itself. */
static int poi_target_count(void) {
	poi_refresh_target();                   /* throttled internally */
	return poi_ntarget;
}

/* Slot i (0-based) by value; name may be "". Returns false when i is not set. */
static bool poi_get_target_at(int i, double *lat, double *lon, const char **name) {
	poi_refresh_target();
	if (i < 0 || i >= poi_ntarget) return false;
	*lat = poi_targets[i].lat;
	*lon = poi_targets[i].lon;
	if (name) *name = poi_targets[i].name;
	return true;
}


/**
 * Draw the preflight targets as red markers with name and distance labels.
 * Unlike regular POIs they ignore the range limit and the POI toggle.
 * @param v frame view
 */
static void DrawTarget(const poi_view_t *v) {
	int n = poi_target_count();

	for (int i = 0; i < n; i++) {
		poi_target_t *t = &poi_targets[i];
		double x, y, dist;
		if (!poi_project(v, t->lat, t->lon, &t->ele, &t->ele_state, false, &x, &y, &dist))
			continue;

		/* "<name> <dist>" on one line; unnamed slots keep the plain distance. */
		char txt[POI_TARGET_NAME + 16];
		if (t->name[0])
			snprintf(txt, sizeof(txt), "%s %.1fkm", t->name, dist / 1000.0);
		else
			snprintf(txt, sizeof(txt), "%.1fkm", dist / 1000.0);

		int ix = (int)(x + 0.5), iy = (int)(y + 0.5);
		/* Two concentric rings (larger + smaller than the 6px POI marker) so the
		 * target reads as a distinct crosshair against a busy background. */
		drawCircleGS(ix, iy, 9, getcolor(COLOR_RED), 2, false);
		drawCircleGS(ix, iy, 3, getcolor(COLOR_RED), 2, false);
		drawText(txt, (int)x + 12, (int)y - 6, getcolor(COLOR_RED), poi_font_size * 1.2, false, 1, 0);
	}
}

/* A POI that passed projection this frame, queued for back-to-front drawing. */
typedef struct {
	double x, y, dist;
	const char *label;
} poi_vis_t;

/**
 * qsort comparator: farthest first, so nearer POIs are drawn on top.
 * @param a, b poi_vis_t pointers
 * @return <0 when a is farther than b
 */
static int poi_cmp_far_first(const void *a, const void *b) {
	double da = ((const poi_vis_t *)a)->dist, db = ((const poi_vis_t *)b)->dist;
	return (da < db) - (da > db);
}

/**
 * Draw POI markers and targets. Called from draw_Ladder() with the AHI state.
 * @param lat_e7, lon_e7 plane position, degrees * 1e7 (MSP_RAW_GPS)
 * @param alt_m          GPS altitude, metres (MSP_RAW_GPS)
 * @param heading_deg    aircraft yaw / compass heading
 * @param pitch_deg      pitch, AHI display convention
 * @param roll_deg       roll rotation applied to the ladder (Transform_Roll)
 * @param pos_y          AHI centre screen y, including tilt
 * @param f              AHI vertical focal length, pixels
 * @param vFOV_deg       vertical field of view implied by f
 */
static void DrawPOIs(int32_t lat_e7, int32_t lon_e7, int16_t alt_m,
                     int16_t heading_deg, double pitch_deg, double roll_deg,
                     int pos_y, double f, double vFOV_deg) {
	if (poi_enabled < 0) poi_read_config();
	poi_update_home(alt_m);
	if (lat_e7 == 0 && lon_e7 == 0) return;          /* no GPS fix yet */

	poi_view_t v = {0};
	v.lat = lat_e7 / 1e7;
	v.lon = lon_e7 / 1e7;
	v.heading   = poi_filter_heading(heading_deg);
	v.pitch_deg = pitch_deg;
	v.roll_deg  = roll_deg;
	v.pos_y = pos_y;
	v.f     = f;
	/* Horizontal focal length: full screen width over the (anamorphic) HFOV. */
	double HFOV_deg = vFOV_deg * 4.0 / 3.0;
	v.f_h = (OVERLAY_WIDTH * 0.5) / tan(HFOV_deg * 0.5 * POI_D2R);
	v.cx  = OVERLAY_WIDTH / 2.0;

	/* Heights: terrain-referenced when elevation data exists, else above home.
	 * Without a latched home the raw altitude is taken as height above home. */
	uint64_t now = get_time_ms();
	double agl;
	v.h_home   = poi_home_set ? alt_m - poi_home_alt : alt_m;
	v.have_msl = AGL_enabled && terrain_agl_get(&agl, now) &&
	             terrain_agl_get_altitude(&v.plane_msl, now);
	double h = v.have_msl ? agl : v.h_home;

	/* Perspective above perspective_alt_m, with hysteresis against flicker. */
	static bool persp = false;
	if (persp ? h < poi_persp_alt_m - POI_PERSP_HYST
	          : h > poi_persp_alt_m + POI_PERSP_HYST)
		persp = !persp;
	v.persp = persp;

	/* The target is independent of the POI toggle and the range limit. */
	DrawTarget(&v);

	if (poi_enabled == 0) return;

	/* Reload candidates only after meaningful movement. */
	if (!poi_have_load) {
		poi_load_bbox(v.lat, v.lon);
	} else {
		double moved, brg;
		poi_dist_bearing(poi_load_lat, poi_load_lon, v.lat, v.lon, &moved, &brg);
		if (moved > poi_range_m / 4.0) poi_load_bbox(v.lat, v.lon);
	}
	if (poi_enabled == 0 || poi_count == 0) return;

	static poi_vis_t vis[POI_MAX];
	int nvis = 0;
	for (int i = 0; i < poi_count; i++) {
		poi_t *p = &poi_list[i];
		poi_vis_t *q = &vis[nvis];
		if (poi_project(&v, p->lat, p->lon, &p->ele, &p->ele_state, true,
		                &q->x, &q->y, &q->dist)) {
			q->label = p->label;
			nvis++;
		}
	}
	qsort(vis, nvis, sizeof(vis[0]), poi_cmp_far_first);

	for (int i = 0; i < nvis; i++) {
		poi_vis_t *q = &vis[i];
		drawCircleGS((int)(q->x + 0.5), (int)(q->y + 0.5), 6, getcolor(COLOR_WHITE), 2, false);
		drawText(q->label, (int)q->x + 9, (int)q->y - 6, getcolor(COLOR_WHITE), poi_font_size, false, 1, 0);
	}
}
