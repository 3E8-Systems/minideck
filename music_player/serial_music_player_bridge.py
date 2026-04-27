#!/usr/bin/env python3
"""Bridge the minideck music_player sketch to macOS Music / Spotify.

Device -> host commands (one line each):
    NEXT, PREV, PLAY_PAUSE

Host -> device lines (newline-terminated, tab-separated):
    META\t<artist>\t<title>\t<album>
    STATE\t<playing|paused|stopped|none>
    ART\t<w>\t<h>\t<nchunks>\t<hash>
    A\t<idx>\t<base64_rgb565>
    ARTEND\t<hash>
    ARTCLR
    CLR                (full repaint request on HELLO)

Usage:
    ./run_bridge.sh                  # creates .venv, installs deps, runs
    python3 serial_music_player_bridge.py --port /dev/cu.usbserial-...
"""
from __future__ import annotations

import argparse
import base64
import glob
import hashlib
import os
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass
from typing import Optional


# ---- constants -----------------------------------------------------------

ART_W = 140
ART_H = 140
# Chunk the 2-bytes-per-pixel image into ~256-byte slices so each wire line
# (A\t<idx>\t<base64>\n) stays well under the device's 768-char serial buffer.
ART_CHUNK_BYTES = 256


# ---- applescript helpers -------------------------------------------------

def run_osascript_text(source: str, timeout: float = 2.0) -> str:
    try:
        r = subprocess.run(
            ["osascript", "-e", source],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return ""
    if r.returncode != 0:
        return ""
    return (r.stdout or "").strip()


def run_osascript(source: str, timeout: float = 2.0) -> bool:
    try:
        r = subprocess.run(
            ["osascript", "-e", source],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return False
    return r.returncode == 0


def _app_is_running(app: str) -> bool:
    # `it is running` avoids launching a non-running player during polling.
    out = run_osascript_text(
        f'tell application "System Events" to (name of processes) contains "{app}"'
    )
    return out.lower() == "true"


# ---- now-playing poll ----------------------------------------------------

@dataclass
class NowPlaying:
    app: str          # "Music" / "Spotify" / ""
    artist: str
    title: str
    album: str
    state: str        # "playing" / "paused" / "stopped" / "none"
    track_id: str     # stable identity for art caching (persistent id, uri, ...)

    @property
    def track_key(self) -> tuple[str, str, str, str]:
        return (self.app, self.artist, self.title, self.album)


def _np_music() -> Optional[NowPlaying]:
    script = '''
tell application "Music"
  if not (it is running) then return ""
  try
    set s to player state as text
    set a to ""
    set t to ""
    set al to ""
    set pid to ""
    if s is "playing" or s is "paused" then
      set a to artist of current track
      set t to name of current track
      set al to album of current track
      try
        set pid to persistent ID of current track
      end try
    end if
    return s & (ASCII character 9) & a & (ASCII character 9) & t & (ASCII character 9) & al & (ASCII character 9) & pid
  on error
    return ""
  end try
end tell
'''
    out = run_osascript_text(script)
    if not out:
        return None
    parts = out.split("\t")
    while len(parts) < 5:
        parts.append("")
    state, artist, title, album, pid = parts[:5]
    if state not in ("playing", "paused", "stopped"):
        state = "none"
    return NowPlaying("Music", artist, title, album, state, pid)


def _np_spotify() -> Optional[NowPlaying]:
    script = '''
tell application "Spotify"
  if not (it is running) then return ""
  try
    set s to player state as text
    set a to ""
    set t to ""
    set al to ""
    set tid to ""
    if s is "playing" or s is "paused" then
      set a to artist of current track
      set t to name of current track
      set al to album of current track
      try
        set tid to id of current track
      end try
    end if
    return s & (ASCII character 9) & a & (ASCII character 9) & t & (ASCII character 9) & al & (ASCII character 9) & tid
  on error
    return ""
  end try
end tell
'''
    out = run_osascript_text(script)
    if not out:
        return None
    parts = out.split("\t")
    while len(parts) < 5:
        parts.append("")
    state, artist, title, album, tid = parts[:5]
    if state not in ("playing", "paused", "stopped"):
        state = "none"
    return NowPlaying("Spotify", artist, title, album, state, tid)


def fetch_now_playing(preference: list[str]) -> NowPlaying:
    # Preference decides tie-breaks when both apps are open; within each
    # player we only trust values when the app is actually running.
    candidates: list[NowPlaying] = []
    for app in preference:
        if app == "Music":
            if _app_is_running("Music"):
                np = _np_music()
                if np:
                    candidates.append(np)
        elif app == "Spotify":
            if _app_is_running("Spotify"):
                np = _np_spotify()
                if np:
                    candidates.append(np)

    # Prefer a playing source over paused/stopped, then fall back to order.
    for np in candidates:
        if np.state == "playing":
            return np
    for np in candidates:
        if np.state in ("paused", "stopped") and (np.artist or np.title):
            return np
    return NowPlaying("", "", "", "", "none", "")


# ---- transport controls --------------------------------------------------

def transport(app: str, action: str) -> bool:
    verb = {
        "play_pause": "playpause",
        "next":       "next track",
        "prev":       "previous track",
    }[action]
    return run_osascript(f'tell application "{app}" to {verb}')


def transport_fallback(action: str, preference: list[str]) -> None:
    # Prefer whichever preferred app is already running AND has a meaningful
    # state; otherwise just try in order. This keeps PLAY_PAUSE from
    # resurrecting a closed Music.app when Spotify is the one playing.
    np = fetch_now_playing(preference)
    if np.app and np.state in ("playing", "paused", "stopped"):
        if transport(np.app, action):
            return
    for app in preference:
        if _app_is_running(app) and transport(app, action):
            return
    print(f"[bridge] no player handled {action}", file=sys.stderr)


# ---- artwork fetch + RGB565 encode ---------------------------------------

def _artwork_music_to_file(path: str) -> bool:
    # AppleScript writes raw bytes (PNG/JPEG blob) for the current track's
    # first artwork. Empty file on failure.
    script = f'''
tell application "Music"
  if not (it is running) then return "no"
  try
    set artCount to count of artworks of current track
    if artCount < 1 then return "no"
    set rawData to raw data of artwork 1 of current track
    set p to POSIX file "{path}"
    try
      set f to open for access p with write permission
      set eof f to 0
      write rawData to f
      close access f
    on error
      try
        close access p
      end try
      return "no"
    end try
    return "ok"
  on error
    return "no"
  end try
end tell
'''
    out = run_osascript_text(script, timeout=3.0)
    return out == "ok" and os.path.exists(path) and os.path.getsize(path) > 64


def _artwork_spotify_url() -> str:
    script = '''
tell application "Spotify"
  if not (it is running) then return ""
  try
    return artwork url of current track
  on error
    return ""
  end try
end tell
'''
    return run_osascript_text(script)


def _download(url: str, dst: str, timeout: float = 4.0) -> bool:
    try:
        import urllib.request
        req = urllib.request.Request(url, headers={"User-Agent": "minideck/1.0"})
        with urllib.request.urlopen(req, timeout=timeout) as r:
            data = r.read()
        if not data or len(data) < 64:
            return False
        with open(dst, "wb") as f:
            f.write(data)
        return True
    except Exception:
        return False


def _load_resize_to_rgb565(path: str, size: int) -> Optional[bytes]:
    """Load an image file, square-crop + resize to size x size, encode as
    big-endian RGB565 (high byte first per pixel)."""
    try:
        from PIL import Image
    except ImportError:
        print("[bridge] Pillow missing — skipping artwork. Re-run run_bridge.sh.", file=sys.stderr)
        return None

    try:
        with Image.open(path) as im:
            im = im.convert("RGB")
            w, h = im.size
            # Center square crop then resize, so portrait/landscape artwork
            # never ends up stretched.
            side = min(w, h)
            left = (w - side) // 2
            top = (h - side) // 2
            im = im.crop((left, top, left + side, top + side))
            im = im.resize((size, size), Image.LANCZOS)
            pixels = im.tobytes()  # RGB packed
    except Exception as e:
        print(f"[bridge] failed to decode artwork {path}: {e}", file=sys.stderr)
        return None

    # Encode RGB888 -> RGB565 big-endian (2 bytes per pixel: HH LL).
    out = bytearray(size * size * 2)
    j = 0
    for i in range(0, len(pixels), 3):
        r = pixels[i]
        g = pixels[i + 1]
        b = pixels[i + 2]
        v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
        out[j]     = (v >> 8) & 0xFF
        out[j + 1] = v & 0xFF
        j += 2
    return bytes(out)


def fetch_artwork_rgb565(np: NowPlaying) -> Optional[bytes]:
    with tempfile.TemporaryDirectory() as td:
        path = os.path.join(td, "art.bin")
        ok = False
        if np.app == "Music":
            ok = _artwork_music_to_file(path)
        elif np.app == "Spotify":
            url = _artwork_spotify_url()
            if url:
                ok = _download(url, path)
        if not ok:
            return None
        return _load_resize_to_rgb565(path, ART_W)


# ---- device I/O ----------------------------------------------------------

def _sanitize_meta_field(s: str) -> str:
    s = s.replace("\t", " ").replace("\r", " ").replace("\n", " ")
    # Transliterate common unicode the device can't render; anything else
    # gets scrubbed on the device side too but cleaner text here helps.
    repl = {
        "\u2013": "-", "\u2014": "-", "\u2012": "-", "\u2015": "-",
        "\u2018": "'", "\u2019": "'", "\u201a": "'", "\u201b": "'",
        "\u201c": '"', "\u201d": '"', "\u201e": '"', "\u201f": '"',
        "\u2026": "...", "\u00a0": " ",
    }
    for k, v in repl.items():
        s = s.replace(k, v)
    return " ".join(s.split())


def _clip(s: str, max_chars: int) -> str:
    if len(s) <= max_chars:
        return s
    if max_chars <= 3:
        return s[:max_chars]
    return s[: max_chars - 3] + "..."


def _send(ser, line: str) -> None:
    ser.write((line + "\n").encode("ascii", errors="replace"))


def send_meta(ser, np: NowPlaying) -> None:
    a = _clip(_sanitize_meta_field(np.artist), 90)
    t = _clip(_sanitize_meta_field(np.title), 120)
    al = _clip(_sanitize_meta_field(np.album), 90)
    _send(ser, f"META\t{a}\t{t}\t{al}")


def send_state(ser, state: str) -> None:
    if state not in ("playing", "paused", "stopped", "none"):
        state = "none"
    _send(ser, f"STATE\t{state}")


def send_art(ser, rgb565: bytes) -> None:
    if not rgb565:
        _send(ser, "ARTCLR")
        return
    expected = ART_W * ART_H * 2
    if len(rgb565) != expected:
        print(f"[bridge] unexpected art size {len(rgb565)} != {expected}", file=sys.stderr)
        _send(ser, "ARTCLR")
        return

    # Hash lets us short-circuit re-sends and gives ART / ARTEND a matched id.
    h = int.from_bytes(hashlib.sha1(rgb565).digest()[:4], "big") & 0x7FFFFFFF

    chunks = [rgb565[i:i + ART_CHUNK_BYTES]
              for i in range(0, len(rgb565), ART_CHUNK_BYTES)]
    _send(ser, f"ART\t{ART_W}\t{ART_H}\t{len(chunks)}\t{h}")

    for idx, c in enumerate(chunks):
        b64 = base64.b64encode(c).decode("ascii")
        _send(ser, f"A\t{idx}\t{b64}")
        # A tiny flush pause every few chunks keeps the device loop() from
        # starving on serial read while we blast ~80 lines back-to-back.
        if (idx & 0x0F) == 0x0F:
            ser.flush()

    _send(ser, f"ARTEND\t{h}")
    ser.flush()


def request_full_repaint(ser) -> None:
    _send(ser, "CLR")
    ser.flush()


# ---- serial plumbing -----------------------------------------------------

def default_serial_candidates() -> list[str]:
    patterns = [
        "/dev/cu.usbserial-*",
        "/dev/cu.SLAB_USBtoUART*",
        "/dev/cu.wchusbserial*",
        "/dev/cu.usbmodem*",
        "/dev/cu.URT0*",
    ]
    found: list[str] = []
    for p in patterns:
        found.extend(glob.glob(p))
    return sorted(set(found))


def pick_serial_port(explicit: Optional[str]) -> Optional[str]:
    if explicit:
        return explicit if explicit.startswith("/") else f"/dev/{explicit.lstrip('/')}"
    for path in default_serial_candidates():
        return path
    return None


def open_serial(port: str, baud: int, *, read_timeout: float):
    import serial
    return serial.Serial(port, baud, timeout=read_timeout)


def handle_device_line(line: str, preference: list[str]) -> None:
    cmd = line.strip()
    if not cmd:
        return
    if cmd == "NEXT":
        transport_fallback("next", preference)
    elif cmd == "PREV":
        transport_fallback("prev", preference)
    elif cmd == "PLAY_PAUSE":
        transport_fallback("play_pause", preference)
    elif cmd.startswith("HELLO\t"):
        # Handled by main loop so it can reset caches + push a fresh frame.
        pass
    else:
        if cmd.isupper() and "_" in cmd:
            print(f"[bridge] unknown device command: {cmd!r}", file=sys.stderr)


# ---- main ----------------------------------------------------------------

def main() -> int:
    p = argparse.ArgumentParser(description="minideck music_player serial bridge")
    p.add_argument("--port", help="Serial device (auto-detect if omitted)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--players", default="Music,Spotify",
                   help="Comma-separated preference order (default Music,Spotify)")
    p.add_argument("--meta-interval", type=float, default=1.0,
                   help="Seconds between now-playing polls (default 1.0)")
    p.add_argument("--no-art", action="store_true",
                   help="Skip artwork transfer (text-only updates)")
    args = p.parse_args()

    preference = [s.strip() for s in args.players.split(",") if s.strip()]

    port = pick_serial_port(args.port)
    if not port:
        print("No serial port found. Pass --port explicitly.", file=sys.stderr)
        return 1

    try:
        import serial  # noqa: F401
    except ImportError:
        print("pyserial missing — run ./run_bridge.sh so the venv is set up.", file=sys.stderr)
        return 1

    while True:
        try:
            ser = open_serial(port, args.baud, read_timeout=0.05)
        except OSError as e:
            print(f"[bridge] could not open {port}: {e}. Retrying in 2s.", file=sys.stderr)
            time.sleep(2)
            continue

        print(f"[bridge] listening on {port} @ {args.baud}. Ctrl+C to stop.", file=sys.stderr)

        buf = b""
        last_meta_key: Optional[tuple[str, str, str, str]] = None
        last_state: Optional[str] = None
        last_art_identity: Optional[tuple[str, str, str, str, str]] = None
        next_poll = 0.0

        try:
            while True:
                chunk = ser.read(1024)
                if chunk:
                    buf += chunk
                    while b"\n" in buf:
                        line_b, buf = buf.split(b"\n", 1)
                        try:
                            line = line_b.decode("utf-8", errors="replace").rstrip("\r")
                        except Exception:
                            continue
                        if not line:
                            continue
                        if line.startswith("HELLO\t"):
                            # Force a fresh push on every boot so the device
                            # never shows stale "Connect bridge" placeholder.
                            last_meta_key = None
                            last_state = None
                            last_art_identity = None
                            request_full_repaint(ser)
                        else:
                            handle_device_line(line, preference)

                now = time.monotonic()
                if now >= next_poll:
                    next_poll = now + max(0.25, args.meta_interval)
                    np = fetch_now_playing(preference)

                    if np.state != last_state:
                        send_state(ser, np.state)
                        last_state = np.state

                    if np.track_key != last_meta_key:
                        send_meta(ser, np)
                        last_meta_key = np.track_key

                        if not args.no_art:
                            # Identity keys on the source app + track id so we
                            # re-fetch art when a new track starts but not on
                            # simple play/pause events.
                            identity = (np.app, np.track_id, np.artist, np.title, np.album)
                            if identity != last_art_identity:
                                if np.state == "none" or not (np.artist or np.title):
                                    _send(ser, "ARTCLR")
                                    ser.flush()
                                else:
                                    rgb = fetch_artwork_rgb565(np)
                                    if rgb is None:
                                        _send(ser, "ARTCLR")
                                        ser.flush()
                                    else:
                                        send_art(ser, rgb)
                                last_art_identity = identity

        except KeyboardInterrupt:
            print("\n[bridge] exiting.", file=sys.stderr)
            try:
                ser.close()
            except OSError:
                pass
            return 0
        except OSError as e:
            print(f"[bridge] serial error: {e}; reopening.", file=sys.stderr)
            try:
                ser.close()
            except OSError:
                pass
            time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
