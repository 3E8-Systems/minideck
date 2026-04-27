#!/usr/bin/env bash
# Homebrew Python blocks "pip install" on the system interpreter (PEP 668).
# This script creates a project .venv, installs pyserial there, and runs the bridge.
set -euo pipefail
cd "$(dirname "$0")"
if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi
.venv/bin/pip install -q pyserial
exec .venv/bin/python serial_music_bridge.py "$@"
