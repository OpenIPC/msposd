#!/usr/bin/env python3
"""Send fake MSP telemetry to the map bridge so you can see the moving map work
without an aircraft.

Flies a circle around a center point, emitting MSP_RAW_GPS (position + ground
course) and MSP_ATTITUDE (heading) to udp://127.0.0.1:14560 — exactly what
`msposd --out 127.0.0.1:14560` would forward in real use.

  python3 sim_msp.py                       # orbit the last downloaded center
  python3 sim_msp.py --lat 43.14 --lon 27.93 --radius-m 800 --speed-ms 25

Altitude is --home-alt + --height (150 m above home by default). With --arm the
plane sits at --home-alt until armed, so msposd latches that as home, then
climbs at --climb-ms to --height above it.

--roll (default 20) banks the wings ±N° (sine, --roll-period s per swing) for
--roll-cycles swings, then holds level for --roll-steady s, and repeats.
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


def raw_gps(lat, lon, alt_m, course_deg, speed_ms):
    return frame(MSP_RAW_GPS, struct.pack(
        "<BBiihhh",
        3, 14,                              # fix type, sats
        int(lat * 1e7), int(lon * 1e7),     # lat, lon (deg * 1e7)
        int(alt_m),                         # GPS altitude (m)
        int(speed_ms * 100),                # speed (cm/s)
        int(course_deg * 10),               # ground course (decidegrees)
    ))


def attitude(heading_deg, roll_deg=0.0):
    # roll/pitch in decidegrees, yaw in degrees
    return frame(MSP_ATTITUDE, struct.pack("<hhh", int(roll_deg * 10), 0, int(heading_deg)))


def roll_at(t, amp, period, cycles, steady):
    """Roll (deg) at time t: `cycles` sine swings of ±amp, then `steady` s level."""
    swing = period * cycles
    tc = t % (swing + steady)
    return amp * math.sin(2 * math.pi * tc / period) if tc < swing else 0.0


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
    ap.add_argument("--rate", type=float, default=20, help="updates per second")
    ap.add_argument("--arm", action="store_true",
                    help="send ARMED after --arm-delay s (triggers home capture)")
    ap.add_argument("--arm-delay", type=float, default=3.0)
    ap.add_argument("--home-alt", type=float, default=300,
                    help="GPS altitude of home (m); sent until armed with --arm")
    ap.add_argument("--height", type=float, default=150,
                    help="flight height above home (m)")
    ap.add_argument("--climb-ms", type=float, default=15,
                    help="climb rate after arming (m/s)")
    # Heading normally equals course here, which makes drift indicators invisible.
    # --crab offsets the nose from the track, like a wing held into a crosswind.
    ap.add_argument("--crab", type=float, default=0.0,
                    help="degrees the nose points off the track (+ = nose right)")
    ap.add_argument("--crab-period", type=float, default=0.0,
                    help="if set, oscillate the crab angle over this many seconds")
    ap.add_argument("--roll", type=float, default=20.0,
                    help="roll swing amplitude in degrees (0 = wings level)")
    ap.add_argument("--roll-period", type=float, default=2.0,
                    help="seconds per full left-right roll swing")
    ap.add_argument("--roll-cycles", type=int, default=2,
                    help="roll swings before each steady phase")
    ap.add_argument("--roll-steady", type=float, default=4.0,
                    help="seconds of level flight between roll phases")
    args = ap.parse_args()

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dst = (args.host, args.port)
    circumference = 2 * math.pi * args.radius_m
    period = circumference / max(args.speed_ms, 0.1)   # seconds per lap
    dlat_r = args.radius_m / EARTH_M_PER_DEG
    dlon_r = args.radius_m / (EARTH_M_PER_DEG * math.cos(math.radians(args.lat)))

    print(f"Simulating flight: center=({args.lat:.5f},{args.lon:.5f}) "
          f"radius={args.radius_m:.0f}m speed={args.speed_ms:.0f}m/s "
          f"height={args.height:.0f}m above home ({args.home_alt:.0f}m) -> {dst}")
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
        # on the ground until armed (home latch), then climb to --height
        if args.arm:
            climb = max(0.0, t - args.arm_delay - 1.0) * args.climb_ms
            alt = args.home_alt + min(args.height, climb)
        else:
            alt = args.home_alt + args.height
        sock.sendto(raw_gps(lat, lon, alt, course, args.speed_ms), dst)
        roll = roll_at(t, args.roll, args.roll_period, args.roll_cycles, args.roll_steady)
        sock.sendto(attitude(heading, roll), dst)
        if args.arm:
            sock.sendto(status(t >= args.arm_delay), dst)   # disarmed first, then armed
        time.sleep(1.0 / args.rate)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
