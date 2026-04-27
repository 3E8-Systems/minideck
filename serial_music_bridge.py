#!/usr/bin/env python3
"""
Bridge ESP32 serial lines (from music.ino) to macOS media / volume.

The sketch sends one line per encoder click (after debounce):
  VOL_UP, VOL_DOWN, NEXT, PREV, PLAY_PAUSE

The bridge pushes now-playing metadata to the board (for the TFT) as UTF-8 lines:
  M\t<artist>\t<title>
(Tab is the field separator; fields must not contain tabs.)

Usage: Homebrew Python blocks global pip (PEP 668 / externally-managed-environment).
Easiest: from this repo run ./run_music_bridge.sh (creates .venv and installs pyserial).

Manual venv:

  cd /path/to/minideck
  python3 -m venv .venv
  .venv/bin/pip install pyserial
  .venv/bin/python serial_music_bridge.py

If --port is omitted, common USB-serial device names under /dev/cu.* are tried.
"""

from __future__ import annotations

import argparse
import glob
import subprocess
import sys
import time


def run_osascript(source: str) -> bool:
    r = subprocess.run(
        ["osascript", "-e", source],
        capture_output=True,
        text=True,
    )
    return r.returncode == 0


def run_osascript_text(source: str) -> str:
    r = subprocess.run(
        ["osascript", "-e", source],
        capture_output=True,
        text=True,
    )
    if r.returncode != 0:
        return ""
    return (r.stdout or "").strip()


def _now_playing_script(app: str) -> str:
    # Tab between artist and title: matches music.ino parser.
    return f'''
tell application "{app}"
  if not (it is running) then return ""
  try
    if player state is not playing then return ""
    set a to artist of current track
    set t to name of current track
    return (a as string) & (ASCII character 9) & (t as string)
  on error
    return ""
  end try
end tell
'''


def fetch_now_playing(apps: list[str]) -> tuple[str, str]:
    for app in apps:
        out = run_osascript_text(_now_playing_script(app))
        if not out:
            continue
        parts = out.split("\t", 1)
        if len(parts) == 2:
            artist, title = parts[0].strip(), parts[1].strip()
            if artist or title:
                return artist, title
    return "—", "Nothing playing"


def _sanitize_meta_field(s: str) -> str:
    s = s.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    return " ".join(s.split())


def _clip(s: str, max_chars: int = 72) -> str:
    if len(s) <= max_chars:
        return s
    if max_chars <= 3:
        return s[:max_chars]
    return s[: max_chars - 3] + "..."


def send_now_playing_line(ser, artist: str, title: str) -> None:
    a = _clip(_sanitize_meta_field(artist))
    t = _clip(_sanitize_meta_field(title))
    ser.write(f"M\t{a}\t{t}\n".encode("utf-8"))
    ser.flush()


def volume_step(delta: int) -> None:
    # System-wide output volume (0–100).
    d = max(-100, min(100, delta))
    script = f"""
    set s to get volume settings
    set cur to output volume of s
    set n to cur + {d}
    if n < 0 then set n to 0
    if n > 100 then set n to 100
    set volume output volume n
    """
    run_osascript(script)


def transport_for_app(app: str, action: str) -> bool:
    actions = {
        "play_pause": "playpause",
        "next": "next track",
        "prev": "previous track",
    }
    cmd = actions[action]
    return run_osascript(f'tell application "{app}" to {cmd}')


def transport(action: str, apps: list[str]) -> None:
    for app in apps:
        if transport_for_app(app, action):
            return
    print(f"Warning: no player handled {action} (tried: {', '.join(apps)})", file=sys.stderr)


def default_serial_candidates() -> list[str]:
    patterns = [
        "/dev/cu.usbserial-*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.usbmodem*",
        "/dev/cu.URT0*",  # some CP210x
    ]
    found: list[str] = []
    for p in patterns:
        found.extend(glob.glob(p))
    # Stable, unique order
    return sorted(set(found))


def pick_serial_port(explicit: str | None) -> str | None:
    if explicit:
        return explicit if explicit.startswith("/") else f"/dev/{explicit.lstrip('/')}"
    for path in default_serial_candidates():
        return path
    return None


def open_serial(port: str, baud: int, *, read_timeout: float):
    import serial

    return serial.Serial(port, baud, timeout=read_timeout)


def handle_line(line: str, players: list[str]) -> None:
    cmd = line.strip()
    if not cmd:
        return
    if cmd == "VOL_UP":
        volume_step(6)
    elif cmd == "VOL_DOWN":
        volume_step(-6)
    elif cmd == "NEXT":
        transport("next", players)
    elif cmd == "PREV":
        transport("prev", players)
    elif cmd == "PLAY_PAUSE":
        transport("play_pause", players)
    else:
        # Ignore boot / debug lines from the sketch (e.g. "TFT Init Complete").
        if cmd.isupper() and "_" in cmd:
            print(f"Ignoring unknown command: {cmd!r}", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description="Serial → macOS media bridge for music.ino")
    parser.add_argument(
        "--port",
        help="Serial device (e.g. /dev/cu.usbserial-0001). Auto-detect if omitted.",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default 115200)")
    parser.add_argument(
        "--players",
        default="Music,Spotify",
        help="Comma-separated AppleScript app names for transport keys (default: Music,Spotify)",
    )
    parser.add_argument(
        "--meta-interval",
        type=float,
        default=1.5,
        metavar="SEC",
        help="How often to poll now playing and send M\\tartist\\ttitle to the board (default: 1.5)",
    )
    parser.add_argument(
        "--no-meta",
        action="store_true",
        help="Do not poll or send now-playing lines (TFT keeps placeholder until enabled)",
    )
    args = parser.parse_args()
    players = [p.strip() for p in args.players.split(",") if p.strip()]

    port = pick_serial_port(args.port)
    if not port:
        print(
            "No serial port found. Connect the board via USB and pass --port explicitly.\n"
            "Example: python3 serial_music_bridge.py --port /dev/cu.usbserial-0001",
            file=sys.stderr,
        )
        return 1

    try:
        import serial  # noqa: F401
    except ImportError:
        print(
            "Install pyserial in a venv, e.g.:\n"
            "  python3 -m venv .venv && .venv/bin/pip install pyserial\n"
            "Then run: .venv/bin/python serial_music_bridge.py",
            file=sys.stderr,
        )
        return 1

    while True:
        try:
            ser = open_serial(port, args.baud, read_timeout=0.05)
        except OSError as e:
            print(f"Could not open {port}: {e}. Retrying in 2s…", file=sys.stderr)
            time.sleep(2)
            continue

        print(f"Listening on {port} at {args.baud} baud. Ctrl+C to stop.", file=sys.stderr)
        buf = b""
        last_meta: tuple[str, str] | None = None
        next_meta_poll = 0.0
        try:
            while True:
                chunk = ser.read(4096)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line, buf = buf.split(b"\n", 1)
                        try:
                            text = line.decode("utf-8", errors="replace").strip()
                        except Exception:
                            continue
                        text = text.rstrip("\r")
                        if text:
                            handle_line(text, players)

                now = time.monotonic()
                if not args.no_meta and now >= next_meta_poll:
                    next_meta_poll = now + max(0.25, args.meta_interval)
                    artist, title = fetch_now_playing(players)
                    key = (artist, title)
                    if key != last_meta:
                        last_meta = key
                        send_now_playing_line(ser, artist, title)
        except KeyboardInterrupt:
            print("\nExiting.", file=sys.stderr)
            ser.close()
            return 0
        except OSError as e:
            print(f"Serial error: {e}. Reopening…", file=sys.stderr)
            try:
                ser.close()
            except OSError:
                pass
            time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
