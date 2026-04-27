#!/usr/bin/env bash
# Bootstrap a project .venv with everything the Claude bridge needs and run it.
# Pass extra args (e.g. an explicit serial port) through to the script.
#
#   ./run_claude_bridge.sh                       # autodetect port
#   ./run_claude_bridge.sh /dev/cu.usbserial-X
#   ./run_claude_bridge.sh --no-stt              # taps only, skip whisper
set -euo pipefail
cd "$(dirname "$0")"

if [[ ! -d .venv ]]; then
  python3 -m venv .venv
fi

# Core deps. faster-whisper is heavy (downloads CT2 + a model on first run);
# omit it if you only want tap dispatch and pass --no-stt.
.venv/bin/pip install -q \
  pyserial \
  sounddevice \
  numpy \
  pyobjc-framework-Cocoa \
  pyobjc-framework-ApplicationServices

# Optional: local STT. Skip if --no-stt is in the args.
if [[ ! " $* " =~ " --no-stt " ]]; then
  .venv/bin/pip install -q faster-whisper || {
    echo "[run] faster-whisper install failed; you can still run with --no-stt"
    echo "[run] or set OPENAI_API_KEY for cloud transcription."
  }
fi

exec .venv/bin/python serial_claude_bridge.py "$@"
