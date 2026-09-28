"""Exercise a frozen hybrid app with generated fixtures and a real pmtiles extraction.

Vectors are served from a localhost HTTP server with byte-range support, so the
extractor runs exactly as for a remote Protomaps build. When gs/assets/bin holds
no extractor, the frozen app downloads it on first use (network required).
"""

from configparser import ConfigParser
import gzip
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import os
from pathlib import Path
import re
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'gs'))
from test_hybrid import encode_fixture, satellite
from pmtiles.writer import Writer
from pmtiles.tile import Compression, TileType, zxy_to_tileid
from protomaps_source import bounds_for_tiles
from PIL import Image


def fixtures(directory):
    """Create local vector and satellite fixtures in directory; return (vector path, maps dir)."""
    vector = directory / 'vectors.pmtiles'
    encoded, names = encode_fixture()
    with vector.open('wb') as f:
        writer = Writer(f)
        writer.write_tile(zxy_to_tileid(15, 100, 100), gzip.compress(encoded))
        writer.finalize({'tile_type': TileType.MVT, 'tile_compression': Compression.GZIP},
                        {'version': '4.0.0', 'vector_layers': [{'id': k} for k in names]})
    maps = directory / 'maps'
    maps.mkdir()
    with sqlite3.connect(maps / 'Satellite.mbtiles') as conn:
        conn.executescript('CREATE TABLE metadata(name,value); CREATE TABLE tiles(zoom_level,tile_column,tile_row,tile_data);')
        conn.execute('INSERT INTO metadata VALUES(?,?)', ('sources', '15:Satellite,17:Satellite'))
        for z, base in ((15, 100), (17, 400)):
            for x in range(base, base + 4):
                for y in range(base, base + 4):
                    conn.execute('INSERT INTO tiles VALUES(?,?,?,?)', (z, x, 2**z - 1 - y, satellite(z, x, y)))
    return vector, maps


def serve_ranges(path):
    """Serve file path over localhost HTTP with Range support; return the running server."""
    data = Path(path).read_bytes()

    class Handler(BaseHTTPRequestHandler):
        """Answer GET/HEAD for the fixture archive, honouring single byte ranges."""

        def do_HEAD(self):
            """Send headers for the whole archive."""
            self.send_response(200)
            self.send_header('Content-Length', str(len(data)))
            self.send_header('Accept-Ranges', 'bytes')
            self.end_headers()

        def do_GET(self):
            """Send the requested byte range, or the whole archive without a Range header."""
            match = re.fullmatch(r'bytes=(\d+)-(\d*)', self.headers.get('Range', ''))
            if match:
                start = int(match[1])
                end = min(int(match[2]) if match[2] else len(data) - 1, len(data) - 1)
                body = data[start:end + 1]
                self.send_response(206)
                self.send_header('Content-Range', f'bytes {start}-{end}/{len(data)}')
            else:
                body = data
                self.send_response(200)
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Accept-Ranges', 'bytes')
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *args):
            """Keep smoke-test output quiet."""

    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    return server


def request(port, path, body=None):
    """Return decoded JSON from localhost port/path, optionally POSTing JSON body."""
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(f'http://127.0.0.1:{port}/{path}', data=data,
                                 headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=10) as response:
        return json.load(response)


def main():
    """Run the supplied executable against isolated fixtures; raise on smoke-test failure."""
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory() as temp:
        directory = Path(temp)
        target = directory / binary.name
        shutil.copy2(binary, target)
        vector, maps = fixtures(directory)
        tool = 'pmtiles.exe' if os.name == 'nt' else 'pmtiles'
        if (ROOT / 'gs/assets/bin' / tool).is_file():
            (directory / 'assets/bin').mkdir(parents=True)
            shutil.copy2(ROOT / 'gs/assets/bin' / tool, directory / 'assets/bin' / tool)
        vectors = serve_ranges(vector)
        config = ConfigParser()
        config['server'] = {'protomaps_source': f'http://127.0.0.1:{vectors.server_port}/vectors.pmtiles',
                            'mbtiles': str(maps / 'area.mbtiles'), 'udp_listen': '127.0.0.1:0'}
        config['map'] = {'basemap': 'Satellite Hybrid', 'hybrid_satellite_pack': 'Satellite', 'elevation': '0',
                         'hybrid_style': json.dumps({'hillshade': False})}   # keep the smoke test offline
        with (directory / 'config.ini').open('w') as f:
            config.write(f)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        with (directory / 'server.log').open('w') as log:
            process = subprocess.Popen([str(target), '--port', str(port), '--no-browser'], stdout=log, stderr=log)
            try:
                for _ in range(100):
                    try:
                        status = request(port, 'status')
                        break
                    except OSError:
                        if process.poll() is not None:
                            raise RuntimeError((directory / 'server.log').read_text())
                        time.sleep(.1)
                else:
                    raise RuntimeError('Frozen server did not start')
                assert 'Satellite Hybrid' not in status['basemaps_disabled'], status
                west, south, east, north = bounds_for_tiles(17, 400, 400, 401, 401)
                state = request(port, 'hybrid/preview', dict(north=north, south=south, east=east, west=west, zoom=17))
                token = state['id']
                for _ in range(1200):              # allows the first-use extractor download
                    if state['state'] != 'loading':
                        break
                    time.sleep(.1)
                    state = request(port, 'hybrid/preview?id=' + token)
                assert state['state'] == 'ready', state
                assert (directory / 'assets/bin' / tool).is_file(), 'extractor was not installed'
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/tiles/17/400/400?hybrid={token}', timeout=20) as response:
                    data = response.read()
                image = Image.open(io.BytesIO(data))
                assert image.size == (256, 256) and image.format == 'JPEG'
                print('Frozen hybrid smoke passed: dependencies, fonts, pmtiles extraction, overzoom, satellite reuse, JPEG endpoint')
            except BaseException:
                print((directory / 'server.log').read_text(errors='replace'))
                raise
            finally:
                vectors.shutdown()
                process.terminate()
                try:
                    process.wait(timeout=15)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()


if __name__ == '__main__':
    main()
