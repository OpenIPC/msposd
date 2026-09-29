#!/usr/bin/env python3
"""Prepare an isolated Python environment with the pinned hybrid-map dependencies."""

from pathlib import Path
import os
import subprocess
import sys
import venv


def main():
    """Install pinned hybrid dependencies beside gs; return nothing and raise on failure."""
    if sys.version_info < (3, 10):
        raise SystemExit('Hybrid maps require Python 3.10 or newer')
    root = Path(__file__).resolve().parent
    environment = root / '.venv-hybrid'
    venv.EnvBuilder(with_pip=True).create(environment)
    python = environment / ('Scripts/python.exe' if os.name == 'nt' else 'bin/python')
    subprocess.run([str(python), '-m', 'pip', 'install', '-r', str(root / 'requirements-hybrid.txt')], check=True)
    print('Hybrid components installed. Restart map preflight to enable Satellite Hybrid.')


if __name__ == '__main__':
    main()
