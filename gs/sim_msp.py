#!/usr/bin/env python3
"""Send fake MSP telemetry to the map bridge so you can see the moving map work
without an aircraft.

Flies a circle around a center point, emitting MSP_RAW_GPS (position + ground
course) and MSP_ATTITUDE (heading) to udp://127.0.0.1:14560 — exactly what
`msposd --out 127.0.0.1:14560` would forward in real use.

  python3 sim_msp.py                       # orbit the last downloaded center
  python3 sim_msp.py --lat 43.14 --lon 27.93 --radius-m 800 --speed-ms 25
"""

import argparse
import math
import os
import socket
import struct
import time
from configparser import ConfigParser

HERE = os.path.dirname(os.path.abspath(__file__))

MSP_RAW_GPS = 106
MSP_ATTITUDE = 108
MSP_STATUS = 101
EARTH_M_PER_DEG = 111320.0


def frame(cmd, payload):
    body = bytes([len(payload), cmd]) + payload
    crc = 0
    for b in body:
        crc ^= b
    return b"$M>" + body + bytes([crc])


def raw_gps(lat, lon, course_deg, speed_ms):
    return frame(MSP_RAW_GPS, struct.pack(
        "<BBiihhh",
        3, 14,                              # fix type, sats
        int(lat * 1e7), int(lon * 1e7),     # lat, lon (deg * 1e7)
        300,                                # altitude (m)
        int(speed_ms * 100),                # speed (cm/s)
        int(course_deg * 10),               # ground course (decidegrees)
    ))


def attitude(heading_deg):
    return frame(MSP_ATTITUDE, struct.pack("<hhh", 0, 0, int(heading_deg)))


def status(armed):
    # cycleTime, i2cErrors, sensors, flightModeFlags(bit0 = ARM)
    return frame(MSP_STATUS, struct.pack("<HHHI", 0, 0, 0, 1 if armed else 0))


def config_center():
    cfg = ConfigParser()
    cfg.read(os.path.join(HERE, "config.ini"))
    try:
        return float(cfg["map"]["center_lat"]), float(cfg["map"]["center_lon"])
    except (KeyError, ValueError):
        return 43.14, 27.93


def main():
    clat, clon = config_center()
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=14560)
    ap.add_argument("--lat", type=float, default=clat)
    ap.add_argument("--lon", type=float, default=clon)
    ap.add_argument("--radius-m", type=float, default=800)
    ap.add_argument("--speed-ms", type=float, default=25)
    ap.add_argument("--rate", type=float, default=5, help="updates per second")
    ap.add_argument("--arm", action="store_true",
                    help="send ARMED after --arm-delay s (triggers home capture)")
    ap.add_argument("--arm-delay", type=float, default=3.0)
    # Heading normally equals course here, which makes drift indicators invisible.
    # --crab offsets the nose from the track, like a wing held into a crosswind.
    ap.add_argument("--crab", type=float, default=0.0,
                    help="degrees the nose points off the track (+ = nose right)")
    ap.add_argument("--crab-period", type=float, default=0.0,
                    help="if set, oscillate the crab angle over this many seconds")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, args.port)
    circumference = 2 * math.pi * args.radius_m
    period = circumference / max(args.speed_ms, 0.1)   # seconds per lap
    dlat_r = args.radius_m / EARTH_M_PER_DEG
    dlon_r = args.radius_m / (EARTH_M_PER_DEG * math.cos(math.radians(args.lat)))

    print(f"Simulating flight: center=({args.lat:.5f},{args.lon:.5f}) "
          f"radius={args.radius_m:.0f}m speed={args.speed_ms:.0f}m/s -> {dst}")
    print("Open the map (./run-map.sh) and watch the plane. Ctrl-C to stop.")

    t0 = time.time()
    while True:
        t = time.time() - t0
        ang = 2 * math.pi * (t / period)            # position angle on the circle
        lat = args.lat + dlat_r * math.cos(ang)
        lon = args.lon + dlon_r * math.sin(ang)
        # course is tangent to the circle (direction of travel)
        course = (math.degrees(ang) + 90) % 360
        # the nose may sit off the track — that gap is what a drift indicator shows
        crab = args.crab
        if args.crab_period > 0:
            crab *= math.sin(2 * math.pi * t / args.crab_period)
        heading = (course + crab) % 360
        sock.sendto(raw_gps(lat, lon, course, args.speed_ms), dst)
        sock.sendto(attitude(heading), dst)
        if args.arm:
            sock.sendto(status(t >= args.arm_delay), dst)   # disarmed first, then armed
        time.sleep(1.0 / args.rate)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
