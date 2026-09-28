"""Render the small Satellite Hybrid style into ordinary raster map tiles."""

import hashlib
from array import array
import io
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

from protomaps_source import GROUP, PAD

STYLE_VERSION = 7
BUFFER = 256
LAYER_ORDER = {'buildings': 0, 'roads': 1, 'places': 2}

# OSM highway/railway value (Protomaps kind_detail) -> drawing class. Left out:
# settlement streets (residential, living_street), service roads and unpaved
# tracks/paths, which only clutter imagery that already shows them.
ROAD_CLASS = {
    'motorway': 'motorway', 'motorway_link': 'motorway', 'trunk': 'motorway', 'trunk_link': 'motorway',
    'primary': 'main', 'primary_link': 'main', 'secondary': 'main', 'secondary_link': 'main',
    'tertiary': 'tertiary', 'tertiary_link': 'tertiary',
    'unclassified': 'street',
    'footway': 'footway', 'sidewalk': 'footway', 'steps': 'footway', 'crossing': 'footway',
    'cycleway': 'footway', 'pedestrian': 'footway',
    'rail': 'rail', 'narrow_gauge': 'rail', 'light_rail': 'rail',
}
# class: (first zoom, fill RGBA, width at z14-15, width at z16+, dark casing)
ROAD_STYLE = {
    'motorway': (8, (245, 170, 80, 240), 4, 5, True),
    'main': (10, (250, 220, 130, 235), 3, 4, True),
    'tertiary': (10, (250, 240, 195, 230), 2, 3, True),
    'street': (10, (250, 250, 245, 225), 1, 2, True),
    'footway': (17, (175, 165, 150, 170), 1, 1, False),
    'rail': (12, (235, 235, 235, 220), 1, 1, False),     # drawn dashed over a grey line
}
LABELLED = {'motorway', 'main', 'tertiary', 'street'}

TERRAIN_ZOOM = 12            # ~30 m terrarium posting; higher zooms only interpolate
SHADE_LIGHT = 0.2            # opacity of white on slopes facing the light
CONTOUR_STEPS = ((11, 100), (13, 50), (15, 20), (99, 10))   # (up to zoom, interval in metres)
CONTOUR_COLOUR = (200, 200, 200)        # alpha comes from the contour_alpha setting (percent)


def intersects(a, b):
    """Return whether pixel rectangles a and b overlap."""
    return a[0] < b[2] and a[2] > b[0] and a[1] < b[3] and a[3] > b[1]


def dashes(line, on, off):
    """Split a polyline into dashes.

    line: List of (x, y) pixel points. on/off: Dash and gap lengths in pixels.
    Returns: List of polylines to draw, with the pattern continuing across vertices.
    """
    pieces, current, drawn, left = [], [line[0]], True, on
    for a, b in zip(line, line[1:]):
        length = math.dist(a, b)
        at = 0.0
        while length - at > left:
            at += left
            point = (a[0] + (b[0] - a[0]) * at / length, a[1] + (b[1] - a[1]) * at / length)
            if drawn:
                pieces.append(current + [point])
            current, drawn = [point], not drawn
            left = on if drawn else off
        left -= length - at
        current.append(b)
    if drawn and len(current) > 1:
        pieces.append(current)
    return pieces


def elevation_samples(terrain, dz, ox, oy, w, h):
    """Return h rows of w elevations in metres, starting at terrain sample ox/oy of zoom dz.

    terrain: Object with tile(z, x, y) returning 256 x 256 row-major elevations.
    Columns wrap around the dateline; rows are clamped at the poles.
    """
    n = 256 * 2 ** dz
    rows = []
    for j in range(h):
        py = min(max(oy + j, 0), n - 1)
        row, x = [], ox
        while len(row) < w:
            px = x % n
            take = min(256 - px % 256, w - len(row))
            start = (py % 256) * 256 + px % 256
            row.extend(terrain.tile(dz, px // 256, py // 256)[start:start + take])
            x += take
        rows.append(row)
    return rows


def contour_lines(terrain, z, gx, gy):
    """Trace contour segments for output group gx/gy at zoom z.

    terrain: As for elevation_samples.
    Returns: [((x0, y0), (x1, y1))] segments in world output pixels.
    Samples are box-blurred against 30 m noise and resampled on a world-aligned grid at most
    8 px apart, so neighbouring groups trace identical segments along their shared edge.
    """
    interval = next(i for top, i in CONTOUR_STEPS if z <= top)
    dz = min(z - 2, TERRAIN_ZOOM)
    f = 2 ** (z - dz)                                  # output pixels per terrain sample
    r = max(1, f // 8)                                 # grid points per terrain sample
    step = f / r
    count = GROUP * 256 // f
    raw = elevation_samples(terrain, dz, gx * 256 // f - 3, gy * 256 // f - 3, count + 6, count + 6)
    size = count + 4                                   # blurred samples, 2-sample margin
    smooth = array('f', (sum(raw[j + b][i + a] for a in (-1, 0, 1) for b in (-1, 0, 1)) / 9
                         for j in range(1, size + 1) for i in range(1, size + 1)))
    grid = Image.frombytes('F', (size, size), smooth.tobytes())
    if r > 1:
        grid = grid.resize((size * r, size * r), Image.Resampling.BICUBIC)
    # Grid point u sits at world pixel gx*256 - 2f + (u + 0.5) * step; keep one point
    # beyond each group edge so the cells cover the whole group.
    m = count * r + 2
    grid = grid.crop((2 * r - 1, 2 * r - 1, 2 * r - 1 + m, 2 * r - 1 + m))
    values = array('f', grid.tobytes())
    g = [values[j * m:(j + 1) * m] for j in range(m)]
    x0, y0 = gx * 256 - step / 2, gy * 256 - step / 2
    segments = []
    for j in range(m - 1):
        upper, lower = g[j], g[j + 1]
        for i in range(m - 1):
            a, b, c, d = upper[i], upper[i + 1], lower[i + 1], lower[i]
            lo, hi = min(a, b, c, d), max(a, b, c, d)
            level = (math.floor(lo / interval) + 1) * interval
            while level <= hi:
                # Corners at or above level are inside; edges: top, right, bottom, left.
                px, py = x0 + i * step, y0 + j * step
                corners = ((px, py, a), (px + step, py, b), (px + step, py + step, c), (px, py + step, d))
                cross = {}
                for e in range(4):
                    (xp, yp, vp), (xq, yq, vq) = corners[e], corners[(e + 1) % 4]
                    if (vp >= level) != (vq >= level):
                        t = (level - vp) / (vq - vp)
                        cross[e] = (xp + (xq - xp) * t, yp + (yq - yp) * t)
                if len(cross) == 2:
                    segments.append(tuple(cross.values()))
                elif len(cross) == 4:              # saddle: the centre decides which corners join
                    pairs = ((0, 1), (2, 3)) if ((a + b + c + d) / 4 >= level) == (a >= level) else ((3, 0), (1, 2))
                    segments.extend((cross[p], cross[q]) for p, q in pairs)
                level += interval
    return segments


def shade_layer(terrain, z, gx, gy, relief, dark):
    """Return an RGBA hillshade for output group gx/gy at zoom z, or None on flat ground.

    terrain: Object with tile(z, x, y) returning 256 x 256 row-major elevations in metres.
    relief: Height exaggeration at z13; it doubles every two zooms, capped at twice relief
    because noise in the 30 m data then shows as fake bumps. dark: Shadow opacity in percent.
    Light comes from the north-west at 45 degrees; flat ground stays transparent.
    One extra shaded sample around the group keeps upscaling seamless between groups.
    """
    dz = min(z - 2, TERRAIN_ZOOM)                      # at most 256 x 256 samples per group
    f = 2 ** (z - dz)                                  # output pixels per terrain sample
    n = 256 * 2 ** dz
    size = GROUP * 256 // f + 2                        # shaded samples, with a 1-sample margin
    ox, oy = gx * 256 // f - 2, gy * 256 // f - 2      # first elevation sample (2-sample margin)
    grid = elevation_samples(terrain, dz, ox, oy, size + 2, size + 2)
    exaggeration = min(2 * relief, relief * 2 ** ((z - 13) / 2))
    shadow = dark / 100
    az, alt = math.radians(315), math.radians(45)
    lx, ly, lz = math.sin(az) * math.cos(alt), math.cos(az) * math.cos(alt), math.sin(alt)
    out = bytearray()
    for j in range(1, size + 1):
        above, row, below = grid[j - 1], grid[j], grid[j + 1]
        # Slope scale from this row's own latitude, so neighbouring groups agree exactly.
        lat = math.atan(math.sinh(math.pi * (1 - 2 * (oy + j + 0.5) / n)))
        k = exaggeration / (2 * 40075016.686 * math.cos(lat) / n)
        for i in range(1, size + 1):
            east = (row[i + 1] - row[i - 1]) * k          # rise per metre eastward
            north = (above[i] - below[i]) * k             # rows run southward
            lit = (lz - lx * east - ly * north) / math.sqrt(1 + east * east + north * north) / lz
            if lit < 1:
                out += bytes((0, 0, 0, round(min(1.0, 1 - lit) * shadow * 255)))
            else:
                out += bytes((255, 255, 255, round(min(1.0, (lit - 1) / (1 / lz - 1)) * SHADE_LIGHT * 255)))
    shade = Image.frombytes('RGBA', (size, size), bytes(out))
    if f > 1:
        shade = shade.resize((size * f, size * f), Image.Resampling.BILINEAR)
    shade = shade.crop((f, f, f + GROUP * 256, f + GROUP * 256))
    return shade if shade.getbbox() else None


def features(source, z, gx, gy, layers=tuple(LAYER_ORDER)):
    """Yield (layer, properties, type, parts, identity) near output group gx/gy at z.

    source: Vector source with maxzoom and tile(z, x, y). layers: Layer names to read.
    Parts are lists of global output-pixel points; identity is a stable feature digest.
    """
    sz = min(z, source.maxzoom)
    factor = 2 ** (z - sz)
    seen = set()
    world = 2 ** sz
    for tx in range(math.floor((gx - PAD) / factor), math.ceil((gx + GROUP + PAD) / factor)):
        for ty in range(max(0, math.floor((gy - PAD) / factor)),
                        min(world, math.ceil((gy + GROUP + PAD) / factor))):
            tile = source.tile(sz, tx % world, ty)
            for name in layers:
                layer = tile.get(name, {})
                scale = 256 * factor / layer.get('extent', 4096)
                ox, oy = tx * 256 * factor, ty * 256 * factor
                for feature in layer.get('features', []):
                    props = feature['properties']
                    if float(props.get('min_zoom', 0)) > z:
                        continue
                    parts = [[(px * scale + ox, py * scale + oy) for px, py in part]
                             for part in feature['parts']]
                    # Features repeated in neighbouring tile buffers project to identical
                    # geometry; differently clipped copies are retained.
                    identity = hashlib.sha1(repr((name, feature['id'], sorted(props.items()),
                                                  parts)).encode()).hexdigest()
                    if identity in seen:
                        continue
                    seen.add(identity)
                    yield name, props, feature['type'], parts, identity


def render_overlay(source, z, gx, gy, settings, font_dir, terrain=None):
    """Draw hillshade, contours, buildings, roads and labels for output group gx/gy at zoom z.

    source: Vector source. settings: Style switches. font_dir: Folder holding DejaVuSans.ttf.
    terrain: Elevation source for hillshade and contours (see elevation_samples), or None to skip them.
    Returns: RGBA image of GROUP x GROUP tiles, or None when nothing is drawn.
    """
    size = GROUP * 256 + 2 * BUFFER
    origin = (gx * 256 - BUFFER, gy * 256 - BUFFER)
    overlay = Image.new('RGBA', (size, size))
    if settings['hillshade'] and terrain is not None:
        shade = shade_layer(terrain, z, gx, gy, settings['shade_relief'], settings['shade_dark'])
        if shade is not None:
            overlay.paste(shade, (BUFFER, BUFFER))
    draw = ImageDraw.Draw(overlay)
    regular = str(Path(font_dir) / 'DejaVuSans.ttf')
    fonts = {12: ImageFont.truetype(regular, 12), 14: ImageFont.truetype(regular, 14)}
    labels = []

    def local(points):
        """Return points translated from world coordinates to the render canvas."""
        return [(p[0] - origin[0], p[1] - origin[1]) for p in points]

    def label(text, x, y, angle, priority, font_size, identity):
        """Append a bounded label candidate with text, location, angle, priority, font and identity."""
        text = str(text).strip()
        if not text or len(text) > 80:
            return
        font = fonts[font_size]
        box = font.getbbox(text, stroke_width=2)
        w, h = box[2] - box[0] + 8, box[3] - box[1] + 8
        if w > 220:
            return
        r = math.radians(angle)
        bw, bh = abs(w * math.cos(r)) + abs(h * math.sin(r)), abs(w * math.sin(r)) + abs(h * math.cos(r))
        bbox = (x - bw / 2 - 4, y - bh / 2 - 4, x + bw / 2 + 4, y + bh / 2 + 4)
        labels.append((priority, identity, text, x, y, angle, font_size, bbox, w, h, box))

    if settings['contours'] and terrain is not None:
        colour = CONTOUR_COLOUR + (round(255 * settings['contour_alpha'] / 100),)
        for p, q in contour_lines(terrain, z, gx, gy):
            draw.line(local([p, q]), fill=colour, width=1)

    wanted = [name for name, on in (('buildings', settings['buildings'] and z >= 15),
                                     ('roads', settings['roads'] or settings['road_names']),
                                     ('places', settings['villages'] or settings['cities'])) if on]
    all_features = sorted(features(source, z, gx, gy, wanted),
                          key=lambda f: (LAYER_ORDER[f[0]], f[1].get('sort_rank', 0), f[4]))
    for layer, props, kind, parts, identity in all_features:
        name = props.get('name') or props.get('name:en') or props.get('name_en')
        if layer == 'buildings' and kind == 3:
            for ring in parts:
                if len(ring) >= 3:
                    draw.line(local(ring + [ring[0]]), fill=(235, 228, 190, 180), width=1)
        elif layer == 'roads' and kind == 2:
            road = ROAD_CLASS.get(props.get('kind_detail'))
            if road is None or z < ROAD_STYLE[road][0]:
                continue
            _, fill, mid, high, casing = ROAD_STYLE[road]
            width = high if z >= 16 else mid if z >= 14 else max(1, mid - 1)
            for line in parts:
                if len(line) < 2:
                    continue
                if settings['roads']:
                    points = local(line)
                    if road == 'rail':
                        draw.line(points, fill=(90, 90, 90, 220), width=3)
                        for piece in dashes(points, 6, 6):
                            draw.line(piece, fill=fill, width=1)
                    else:
                        if casing:
                            draw.line(points, fill=(30, 30, 30, 210), width=width + 2, joint='curve')
                        draw.line(points, fill=fill, width=width, joint='curve' if width > 1 else None)
                if settings['road_names'] and name and z >= 14 and road in LABELLED:
                    length = fonts[12].getlength(str(name)) + 20
                    candidates = [(math.dist(a, b), a, b) for a, b in zip(line, line[1:])]
                    distance, a, b = max(candidates)
                    if distance >= length:
                        x, y = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
                        angle = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
                        if angle > 90:
                            angle -= 180
                        if angle < -90:
                            angle += 180
                        label(name, x, y, angle, 2, 12, f'{name}:{x:.2f}:{y:.2f}')
        elif layer == 'places' and kind == 1 and name and parts:
            detail = props.get('kind_detail')
            if ((settings['villages'] and detail in ('village', 'hamlet')) or
                    (settings['cities'] and detail in ('town', 'city'))):
                x, y = parts[0][0]
                label(name, x, y, 0, 0 if detail in ('town', 'city') else 1, 14,
                      f'{name}:{x:.2f}:{y:.2f}')

    # Pairwise priority suppression, rather than order-dependent greedy placement,
    # gives the same decision on either side of a render-group boundary. The
    # maximum label extent is smaller than the neighbouring candidate buffer.
    unique = {item[1]: item for item in labels}
    ordered = sorted(unique.values(), key=lambda item: (item[0], item[1]))
    grid = {}
    for item in ordered:
        priority, identity, text, x, y, angle, fs, bbox, w, h, box = item
        cells = [(cx, cy) for cx in range(math.floor(bbox[0] / 128), math.floor(bbox[2] / 128) + 1)
                 for cy in range(math.floor(bbox[1] / 128), math.floor(bbox[3] / 128) + 1)]
        blocked = any(intersects(bbox, other) for cell in cells for other in grid.get(cell, []))
        # Even suppressed labels participate so the decision is independent of
        # more distant candidates outside the group buffer.
        for cell in cells:
            grid.setdefault(cell, []).append(bbox)
        if blocked:
            continue
        stamp = Image.new('RGBA', (w, h))
        ImageDraw.Draw(stamp).text((4 - box[0], 4 - box[1]), text, font=fonts[fs],
                                  fill='white', stroke_width=2, stroke_fill=(25, 25, 25, 255))
        stamp = stamp.rotate(-angle, expand=True, resample=Image.Resampling.BICUBIC)
        overlay.alpha_composite(stamp, (round(x - origin[0] - stamp.width / 2),
                                        round(y - origin[1] - stamp.height / 2)))
    overlay = overlay.crop((BUFFER, BUFFER, BUFFER + GROUP * 256, BUFFER + GROUP * 256))
    return overlay if overlay.getbbox() else None


def compose_tile(overlay, gx, gy, x, y, raw):
    """Return JPEG bytes of satellite tile raw at XYZ x/y under its part of a group overlay.

    overlay: Result of render_overlay for group gx/gy (None when empty). raw: 256 x 256 image bytes.
    """
    with Image.open(io.BytesIO(raw)) as image:
        if image.size != (256, 256):
            raise ValueError('Satellite tiles must be 256 x 256')
        tile = image.convert('RGBA')
    if overlay is not None:
        left, top = (x - gx) * 256, (y - gy) * 256
        tile.alpha_composite(overlay.crop((left, top, left + 256, top + 256)))
    output = io.BytesIO()
    tile.convert('RGB').save(output, 'JPEG', quality=90, subsampling=0, progressive=False)
    return output.getvalue()
