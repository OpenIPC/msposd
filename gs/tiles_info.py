#!/usr/bin/env python3
"""Inspect the offline tile cache (maps/area.mbtiles).

  python3 tiles_info.py                 # per-zoom counts + covered lat/lon box
  python3 tiles_info.py --lat 43.14 --lon 27.93   # is this point covered at z11/13/15?

Helps explain "empty box" tiles on the offline map: a blank tile means that exact
z/x/y is not in the cache (never downloaded, download failed, or you panned outside the
downloaded area). Re-running "Download visible area" in preflight fills missing tiles.
"""

import argparse
import glob
import math
import os
import sqlite3
import sys
from configparser import ConfigParser

HERE = os.path.dirname(os.path.abspath(__file__))


def maps_dir():
    cfg = ConfigParser()
    cfg.read(os.path.join(HERE, "config.ini"))
    p = cfg.get("server", "mbtiles", fallback="./maps/area.mbtiles")
    return os.path.dirname(os.path.normpath(os.path.join(HERE, p)))


def num2deg(x, y, z):
    n = 1 << z
    lon = x / n * 360.0 - 180.0
    lat = math.degrees(math.atan(math.sinh(math.pi * (1 - 2 * y / n))))
    return lat, lon


def deg2num(lat, lon, z):
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    y = int((1 - math.asinh(math.tan(math.radians(lat))) / math.pi) / 2 * n)
    return x, y


def box_km(nlat, wlon, slat, elon):
    """Ground size of a lat/lon box in km (x = east-west, y = north-south)."""
    km_y = (nlat - slat) * 111.32
    km_x = (elon - wlon) * 111.32 * math.cos(math.radians((nlat + slat) / 2))
    return abs(km_x), abs(km_y)


def report_elevation(path, lat, lon):
    """Print what the terrain-elevation DB holds, and the height at lat/lon."""
    if not os.path.exists(path):
        return
    try:
        conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
        try:
            meta = dict(conn.execute("SELECT name, value FROM meta").fetchall())
            zoom = int(meta.get("zoom", 12))
            cnt, lo, hi = conn.execute(
                "SELECT COUNT(*), MIN(min_m), MAX(max_m) FROM elevation").fetchone()
        finally:
            conn.close()
    except sqlite3.Error as e:
        print(f"\n=== {os.path.basename(path)} — unreadable ({e})")
        return
    size = os.path.getsize(path)
    print(f"\n=== {os.path.basename(path)}  ({size/1e6:.1f} MB)")
    if not cnt:
        print("  nothing stored yet")
        return
    res = 156543.03392 * math.cos(math.radians(lat if lat is not None else 43)) / 2 ** zoom
    print(f"  z{zoom} · {cnt} tiles · {lo}…{hi} m · ~{res:.0f} m/sample "
          f"· {meta.get('vertical_datum', '?')}")
    if lat is not None and lon is not None:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        try:
            import mapserver
            e = mapserver.elevation_at(lat, lon)
        except Exception as exc:
            print(f"  lookup failed: {exc}")
            return
        print(f"  {lat:.5f},{lon:.5f} -> "
              + (f"{e:.1f} m above sea level" if e is not None else "not downloaded"))


def report(path, lat, lon):
    name = os.path.splitext(os.path.basename(path))[0]
    conn = sqlite3.connect(f"file:{path}?mode=ro", uri=True)
    rows = conn.execute(
        "SELECT zoom_level, COUNT(*), MIN(tile_column), MAX(tile_column), "
        "MIN(tile_row), MAX(tile_row) FROM tiles GROUP BY zoom_level ORDER BY zoom_level"
    ).fetchall()
    sample = conn.execute("SELECT tile_data FROM tiles LIMIT 1").fetchone()
    print(f"=== {name} ({path}) ===")
    if not rows:
        print("  (empty)\n"); return
    fmt = "?"
    if sample and sample[0]:
        fmt = "JPEG" if sample[0][:2] == b"\xff\xd8" else "PNG"
    print(f"  format: {fmt}   size on disk: {os.path.getsize(path) / (1 << 20):.1f} MB")
    total = 0
    print(f"  {'zoom':>4} {'tiles':>7}   covered area (lat,lon NW -> SE){'':>10}   size km")
    for z, cnt, minc, maxc, minr, maxr in rows:
        total += cnt
        ymax = (1 << z) - 1                 # MBTiles rows are TMS; convert to XYZ y
        nlat, wlon = num2deg(minc, ymax - maxr, z)
        slat, elon = num2deg(maxc + 1, (ymax - minr) + 1, z)
        km_x, km_y = box_km(nlat, wlon, slat, elon)
        print(f"  {z:>4} {cnt:>7}   ({nlat:.4f},{wlon:.4f}) -> ({slat:.4f},{elon:.4f})"
              f"   {km_x:.1f} x {km_y:.1f}")
    print(f"  total: {total} tiles")
    if lat is not None and lon is not None:
        print(f"  coverage at ({lat},{lon}):")
        # probe whatever levels this cache actually holds, not a fixed set --
        # the detail zoom is user-selectable ([map] detail_zoom)
        for z in [r[0] for r in rows]:
            x, y = deg2num(lat, lon, z)
            hit = conn.execute(
                "SELECT 1 FROM tiles WHERE zoom_level=? AND tile_column=? AND tile_row=?",
                (z, x, (1 << z) - 1 - y),
            ).fetchone()
            print(f"    z{z:>2}  tile {x}/{y}  {'PRESENT' if hit else 'MISSING'}")
    print()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--lat", type=float)
    ap.add_argument("--lon", type=float)
    ap.add_argument("--basemap", help="inspect only this basemap's cache")
    args = ap.parse_args()

    md = maps_dir()
    files = ([os.path.join(md, args.basemap + ".mbtiles")] if args.basemap
             else sorted(glob.glob(os.path.join(md, "*.mbtiles"))))
    files = [f for f in files if os.path.exists(f)]
    if not files:
        print(f"No tile caches in {md}\nDownload an area in preflight first.")
        return
    for f in files:
        report(f, args.lat, args.lon)
    report_elevation(os.path.join(md, "elevation.db"), args.lat, args.lon)


if __name__ == "__main__":
    main()
