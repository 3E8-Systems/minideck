#!/usr/bin/env bash
# Homebrew Python blocks "pip install" on the system interpreter (PEP 668).
# Create a project-local .venv, install deps (pyserial for the board link,
# Pillow for album-art resize), and run the bridge.
set -euo pipefail
cd "$(dirname "$0")"
if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi
.venv/bin/pip install -q --upgrade pip
.venv/bin/pip install -q pyserial Pillow
exec .venv/bin/python serial_music_player_bridge.py "$@"
