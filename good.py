#!/usr/bin/env python3
"""minideck Claude Code bridge.

Push-to-talk + state-aware approval for Claude Code via the minideck
firmware. macOS only.

Behavior
--------
  - Tap (< HOLD_MS):
      * idle    -> dispatch the currently-selected menu item
      * working -> ESC (interrupt Claude)
      * awaiting approval -> Yes (Return) / Always ("2") / No (ESC),
        chosen by the highlighted menu item.
  - Hold (>= HOLD_MS):
      * record from default mic; on release, transcribe and paste
        + Return into the focused terminal.
      * twist while holding = cancel.
      * disabled in `awaiting` state so you can't accidentally arm
        the mic when Claude is asking permission.

The device sends DOWN / UP / TURN; this script decides what they mean.

Install
-------
  pip install pyserial sounddevice numpy pyobjc
  pip install faster-whisper       # recommended; local + fast
  # OR set OPENAI_API_KEY for cloud transcription as a fallback.

Run
---
  python serial_claude_bridge.py            # autodetects the serial port
  python serial_claude_bridge.py /dev/cu.usbserial-XXXX

First run will trigger macOS permission prompts for the microphone and
for "controlling" Terminal/iTerm via System Events. Grant both.
"""

from __future__ import annotations

import argparse
import glob
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

import numpy as np
import serial
import sounddevice as sd

try:
    from AppKit import NSWorkspace
except Exception:  # pragma: no cover — non-macOS
    NSWorkspace = None  # type: ignore[assignment]

# AX (Accessibility) API — used to read visible text out of apps that
# don't expose AppleScript hooks (Claude desktop, Electron apps, etc.).
# Falls back gracefully if pyobjc-framework-ApplicationServices isn't
# installed; terminal scraping still works without it.
try:
    from ApplicationServices import (
        AXUIElementCreateApplication,
        AXUIElementCopyAttributeValue,
    )
    HAS_AX = True
except Exception:
    HAS_AX = False

try:
    from ApplicationServices import AXIsProcessTrusted
except Exception:
    AXIsProcessTrusted = None  # type: ignore[assignment]


# ---------- config ------------------------------------------------------

BAUD = 115200
HOLD_MS = 300                # tap-vs-hold threshold
SAMPLE_RATE = 16000
RECORD_CHANNELS = 1
RECORD_DTYPE = "int16"
MIN_CLIP_MS = 250            # discard recordings shorter than this
# Apps the bridge is willing to paste into. Add your own if needed — find
# the bundle ID with:
#   osascript -e 'tell application "System Events" to get bundle identifier
#                 of first process whose frontmost is true'
ALLOWED_BUNDLES = {
    # Claude products
    "com.anthropic.claudefordesktop",   # Claude desktop app (and Claude Code in it)
    # Terminals
    "com.apple.Terminal",
    "com.googlecode.iterm2",
    "co.zeit.hyper",
    "com.github.wez.wezterm",
    "net.kovidgoyal.kitty",
    "dev.warp.Warp-Stable",
    "com.mitchellh.ghostty",
    # IDEs (Claude Code in integrated terminal)
    "com.microsoft.VSCode",
    "com.microsoft.VSCodeInsiders",
    "com.todesktop.230313mzl4w4u92",    # Cursor
    "com.jetbrains.intellij",
    "com.jetbrains.pycharm",
}
# The device has exactly two jobs: push-to-talk (hold) and approve/deny
# during Claude's permission prompts (tap). Everything else is intentionally
# absent so there's nothing to accidentally fire.
IDLE_MENU = ("Talk",)
# Shown while Claude is asking for permission. Turn to pick, tap to commit.
# "Always" types "2" to pick Claude's "Yes, and don't ask again" option;
# Deny is wired to ESC so the user can then (optionally) dictate a reason.
AWAIT_MENU = ("Yes", "Always", "No")
# Max glyphs that fit on the device's persistent status area. The firmware
# wraps to two size-1 lines of ~20 chars each, so 40 lets approval
# questions like "edit file.py" render without being sliced in half.
INFO_MAX = 40
SPINNER_CHARS = set("⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏⣾⣽⣻⢿⡿⣟⣯⣷")


# ---------- transcription backends --------------------------------------

class _WhisperLocal:
    """faster-whisper wrapper. Loads once, transcribes synchronously."""

    def __init__(self, model_name: str = "base.en") -> None:
        from faster_whisper import WhisperModel

        # device="auto" picks Metal on Apple Silicon.
        self.model = WhisperModel(model_name, device="auto", compute_type="auto")

    def transcribe(self, audio: np.ndarray, sample_rate: int) -> str:
        if audio.dtype != np.float32:
            audio = audio.astype(np.float32) / 32768.0
        if sample_rate != 16000:
            ratio = 16000 / sample_rate
            n = int(len(audio) * ratio)
            audio = np.interp(
                np.linspace(0, len(audio) - 1, n),
                np.arange(len(audio)),
                audio,
            ).astype(np.float32)
        segments, _info = self.model.transcribe(
            audio, language="en", vad_filter=True,
        )
        return "".join(seg.text for seg in segments).strip()


class _WhisperOpenAI:
    """OpenAI Whisper API fallback. Requires OPENAI_API_KEY."""

    def __init__(self) -> None:
        from openai import OpenAI

        self.client = OpenAI()

    def transcribe(self, audio: np.ndarray, sample_rate: int) -> str:
        import io
        import wave

        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sample_rate)
            w.writeframes(audio.astype(np.int16).tobytes())
        buf.seek(0)
        buf.name = "audio.wav"
        result = self.client.audio.transcriptions.create(
            model="whisper-1",
            file=buf,
        )
        return (result.text or "").strip()


def make_transcriber() -> Callable[[np.ndarray, int], str]:
    try:
        return _WhisperLocal().transcribe
    except Exception as exc:
        print(f"[whisper] local unavailable ({exc}); trying OpenAI", file=sys.stderr)
    if os.environ.get("OPENAI_API_KEY"):
        return _WhisperOpenAI().transcribe
    raise RuntimeError(
        "No transcription backend. Install faster-whisper or set OPENAI_API_KEY."
    )


# ---------- macOS interaction -------------------------------------------

def frontmost_bundle() -> Optional[str]:
    if NSWorkspace is None:
        return None
    app = NSWorkspace.sharedWorkspace().frontmostApplication()
    return None if app is None else str(app.bundleIdentifier())


def frontmost_app_name() -> str:
    if NSWorkspace is None:
        return ""
    app = NSWorkspace.sharedWorkspace().frontmostApplication()
    return "" if app is None else str(app.localizedName())


def applescript(src: str, timeout: float = 2.0) -> str:
    try:
        proc = subprocess.run(
            ["osascript", "-e", src],
            capture_output=True,
            text=True,
            timeout=timeout,
        )
    except subprocess.TimeoutExpired:
        return ""
    if proc.returncode != 0:
        return ""
    return proc.stdout.rstrip("\n")


def get_terminal_visible_text(bundle: str) -> str:
    """Best-effort read of the focused terminal session's contents.

    Returns at most the last ~5 KB. Empty string if unsupported terminal
    or if the user hasn't granted Automation permission yet.
    """
    if bundle == "com.apple.Terminal":
        out = applescript(
            'tell application "Terminal" to get contents of selected tab '
            'of front window'
        )
    elif bundle == "com.googlecode.iterm2":
        out = applescript(
            'tell application "iTerm" to tell current session of current '
            'window to get contents'
        )
    else:
        return ""
    return out[-5000:] if out else ""


def _ax_walk_text(elem, out: list, depth: int = 0, max_depth: int = 30) -> None:
    """Recursively concatenate AXValue / AXTitle strings from an AX element.

    Slow-ish (one AX call per attribute per element), but the focused window
    of a typical app fits in well under a second.
    """
    if depth > max_depth or elem is None:
        return
    for attr in ("AXValue", "AXTitle"):
        try:
            err, val = AXUIElementCopyAttributeValue(elem, attr, None)
        except Exception:
            continue
        if err == 0 and isinstance(val, str) and val.strip():
            out.append(val)
    try:
        err, kids = AXUIElementCopyAttributeValue(elem, "AXChildren", None)
    except Exception:
        return
    if err == 0 and kids:
        for kid in kids:
            _ax_walk_text(kid, out, depth + 1, max_depth)


def get_app_text_via_ax(bundle: str) -> str:
    """Read visible text from the focused app using the macOS Accessibility
    API. Works for Electron apps (Claude desktop, VS Code) where AppleScript
    can't reach the rendered content.

    Requires Accessibility permission for whichever process is running this
    script (System Settings → Privacy & Security → Accessibility). Returns
    "" if AX isn't available, the app isn't focused, or permission is denied.
    """
    if not HAS_AX or NSWorkspace is None:
        return ""
    app = NSWorkspace.sharedWorkspace().frontmostApplication()
    if app is None or str(app.bundleIdentifier()) != bundle:
        return ""
    pid = int(app.processIdentifier())
    try:
        ax_app = AXUIElementCreateApplication(pid)
        err, win = AXUIElementCopyAttributeValue(ax_app, "AXFocusedWindow", None)
    except Exception:
        return ""
    if err != 0 or win is None:
        return ""
    texts: list[str] = []
    try:
        _ax_walk_text(win, texts)
    except Exception:
        return ""
    return "\n".join(texts)[-5000:]


def get_visible_text(bundle: str) -> str:
    """Try AppleScript-friendly terminals first; fall back to the AX tree
    for everything else (Claude desktop, VS Code, Cursor, ...)."""
    out = get_terminal_visible_text(bundle)
    if out:
        return out
    return get_app_text_via_ax(bundle)


def press_escape() -> None:
    applescript('tell application "System Events" to key code 53')


def press_return() -> None:
    applescript('tell application "System Events" to key code 36')


def type_char(c: str) -> None:
    # Only called with hardcoded digits; still escape quotes defensively.
    c = c.replace('"', '\\"')
    applescript(f'tell application "System Events" to keystroke "{c}"')


def paste_via_clipboard(text: str) -> None:
    """Push text to clipboard, then ⌘V into the frontmost app.

    Much more reliable than `keystroke` for arbitrary text (handles
    multi-byte chars, long strings, special chars).
    """
    p = subprocess.Popen(["pbcopy"], stdin=subprocess.PIPE)
    p.communicate(text.encode("utf-8"))
    # tiny pause for the clipboard to settle before the paste
    time.sleep(0.03)
    applescript('tell application "System Events" to keystroke "v" using command down')


# ---------- text-to-speech ----------------------------------------------

_say_lock = threading.Lock()
_say_proc: Optional[subprocess.Popen] = None


def say_stop() -> None:
    """Terminate any in-flight `say` subprocess."""
    global _say_proc
    with _say_lock:
        if _say_proc is not None and _say_proc.poll() is None:
            try:
                _say_proc.terminate()
            except Exception:
                pass
        _say_proc = None


def say_async(text: str, voice: Optional[str] = None) -> None:
    """Speak `text` via macOS `say`. Cancels any previous utterance first."""
    text = (text or "").strip()
    if not text:
        return
    say_stop()
    print(f"[tts] {text!r}", file=sys.stderr)
    cmd = ["say"]
    if voice:
        cmd += ["-v", voice]
    cmd.append(text)
    global _say_proc
    with _say_lock:
        try:
            _say_proc = subprocess.Popen(
                cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
            )
        except Exception as exc:
            print(f"[tts] say failed: {exc}", file=sys.stderr)
            _say_proc = None


# ---------- Claude Code state detection ---------------------------------

# Loose patterns tuned for the current Claude Code TUI. If Claude's UI
# changes these may go stale; the fallback is "idle" which is safe — PTT
# still works, taps just become Return.
RE_AWAITING = re.compile(
    r"(\bDo you want\b|\(y/n\)|\bproceed\?|\b1\.\s+Yes\b|❯\s*1\.)",
    re.IGNORECASE,
)
RE_WORKING = re.compile(
    r"(esc to interrupt|⠋|⠙|⠹|⠸|⠼|⠴|⠦|⠧|⠇|⠏|tokens?\s*[·•]\s*esc)",
    re.IGNORECASE,
)


def detect_claude_state(visible: str) -> tuple[str, str]:
    """Return (state_name, header2_text) from a terminal buffer."""
    if not visible:
        return "idle", "ready"
    tail = "\n".join(visible.splitlines()[-15:])
    if RE_AWAITING.search(tail):
        return "awaiting", "approve?"
    if RE_WORKING.search(tail):
        return "working", "working…"
    return "idle", "ready"


_LEADER_RE = re.compile(r"^[\W_]+")
_TRAILER_RE = re.compile(r"[│┃╎\s]+$")


def _strip_leaders(s: str) -> str:
    # Strip both leading decorators (⏺ ⎿ ❯ │ …) and the trailing box borders
    # that Claude's approval prompt leaves behind when we scrape the terminal.
    s = _LEADER_RE.sub("", s)
    s = _TRAILER_RE.sub("", s)
    return s.strip()


# Parses the "Do you want to …?" line into verb + target. Terminals wrap
# long questions across multiple rows, so we also accept the question
# reassembled from two adjacent lines (see extract_approval_summary).
_APPROVAL_RE = re.compile(
    r"Do you want to\s+"
    r"(?:make this\s+)?"
    r"(?P<verb>edit|create|write|run|proceed|append|apply|execute|delete|remove)"
    r"(?:\s+this(?:\s+command|\s+file)?)?"
    r"(?:\s+to)?"
    r"\s*(?P<target>[^?\n]*)",
    re.IGNORECASE,
)


def extract_last_response(visible: str, last_input: str = "") -> str:
    """Grab Claude's most recent reply (the ⏺ block AFTER the user's
    last input). Returns "" if Claude hasn't responded to the latest
    input yet, or if the buffer doesn't look like a Claude session.

    Two protections against speaking the wrong thing:
      1. Scope is anchored on `last_input` (the verbatim text we just
         pasted into Claude Code). We only consider what comes AFTER it.
         Falls back to the most recent `\n❯ <non-empty>` line so the
         empty input prompt at the bottom doesn't pin scope to nothing.
      2. The walk MUST end on a `⏺` leader, otherwise we return "" —
         this filters out the case where the user has switched to a
         different window and AppleScript/AX is reading something else.
    """
    if not visible or "⏺" not in visible:
        return ""
    scope: Optional[str] = None
    # Prefer anchoring on the user's actual transcript. Use a short
    # leading slice so soft-wrapped lines still match.
    if last_input:
        needle = last_input.strip()[:30]
        if needle:
            idx = visible.rfind(needle)
            if idx >= 0:
                scope = visible[idx + len(needle):]
    if scope is None:
        # Fallback: find the most recent ❯ line that has *content* after
        # the marker. The empty `❯ ` of Claude Code's input box would
        # otherwise pin scope to nothing useful.
        for m in re.finditer(r"\n❯\s+\S", visible):
            scope = visible[m.start():]
        if scope is None:
            scope = visible
    if "⏺" not in scope:
        # New user input is on screen but Claude's reply hasn't rendered
        # yet — caller should retry next tick.
        return ""

    lines = scope.splitlines()[-60:]
    collected: list[str] = []
    total = 0
    MAX_CHARS = 400
    found_content = False
    found_leader = False
    for line in reversed(lines):
        stripped = line.strip()
        if not stripped:
            if found_content:
                break
            continue
        if any(c in SPINNER_CHARS for c in stripped):
            continue
        if "esc to interrupt" in stripped.lower():
            continue
        # Tool-call rows and the input box — not something to speak.
        first = stripped[0]
        if first in "⎿╭╰│>":
            if found_content:
                break
            continue
        clean = _strip_leaders(stripped)
        if len(clean) < 2:
            continue
        if total + len(clean) > MAX_CHARS and found_content:
            break
        collected.insert(0, clean)
        total += len(clean)
        found_content = True
        if stripped.startswith("⏺"):
            found_leader = True
            break
    if not found_leader:
        # Walked off the top without hitting Claude's reply marker —
        # we're almost certainly looking at the wrong buffer.
        return ""
    return " ".join(collected)


def extract_claude_tail(visible: str) -> str:
    """Pick a short, human-readable preview of Claude's latest output.

    Scans recent lines; drops the spinner / "esc to interrupt" hint and
    decorative leaders (⏺ ⎿ ❯). Returns the last line with enough signal,
    or "" if nothing looks printable.
    """
    if not visible:
        return ""
    for raw in reversed(visible.splitlines()[-25:]):
        s = raw.strip()
        if len(s) < 3:
            continue
        if any(c in SPINNER_CHARS for c in s):
            continue
        if "esc to interrupt" in s.lower():
            continue
        s2 = _strip_leaders(s)
        if len(s2) < 3:
            continue
        return s2
    return ""


def extract_question(visible: str) -> str:
    """Pull the "Do you want…?" line Claude is blocking on, if any.

    When Claude's prompt wraps across two box rows the "?" lives on the
    next line, so we also try joining the "Do you want…" line with the
    line below it before giving up.
    """
    if not visible:
        return ""
    lines = visible.splitlines()[-25:]
    for i in range(len(lines) - 1, -1, -1):
        s = _strip_leaders(lines[i])
        if not s:
            continue
        if "Do you" in s:
            if "?" not in s and i + 1 < len(lines):
                nxt = _strip_leaders(lines[i + 1])
                if nxt and not nxt.lower().startswith(("1.", "2.", "3.", "❯")):
                    s = f"{s} {nxt}".strip()
            return s
        if s.endswith("?") and len(s) < 120:
            return s
    return ""


def extract_approval_summary(visible: str) -> str:
    """Short "verb target" string describing what Claude wants approved.

    Falls back to the raw question if we can't parse it, and to "" if
    there's no question in the buffer at all.
    """
    q = extract_question(visible)
    if not q:
        return ""
    m = _APPROVAL_RE.search(q)
    if not m:
        return q
    verb = m.group("verb").lower()
    target = m.group("target").strip().strip("'\"?.:,")
    if verb in ("proceed", "run", "execute") and not target:
        return "run command"
    if "/" in target and len(target) > 24:
        target = target.rsplit("/", 1)[-1]
    if target:
        return f"{verb} {target}"
    return verb


def truncate(s: str, n: int = INFO_MAX) -> str:
    if len(s) <= n:
        return s
    return s[: n - 3] + "..."


# ---------- recorder ----------------------------------------------------

class Recorder:
    """Background mic capture into a chunk list. Thread-safe stop/cancel."""

    def __init__(self, sample_rate: int = SAMPLE_RATE) -> None:
        self.sample_rate = sample_rate
        self._stream: Optional[sd.InputStream] = None
        self._chunks: list[np.ndarray] = []
        self._lock = threading.Lock()
        self.active = False

    def _callback(self, indata, _frames, _t, _status) -> None:
        with self._lock:
            self._chunks.append(indata.copy())

    def start(self) -> None:
        if self.active:
            return
        with self._lock:
            self._chunks = []
        self._stream = sd.InputStream(
            samplerate=self.sample_rate,
            channels=RECORD_CHANNELS,
            dtype=RECORD_DTYPE,
            callback=self._callback,
        )
        self._stream.start()
        self.active = True

    def _close(self) -> None:
        if self._stream is not None:
            try:
                self._stream.stop()
                self._stream.close()
            finally:
                self._stream = None
        self.active = False

    def stop(self) -> Optional[np.ndarray]:
        if not self.active:
            return None
        self._close()
        with self._lock:
            if not self._chunks:
                return None
            return np.concatenate(self._chunks).reshape(-1)

    def cancel(self) -> None:
        if not self.active:
            return
        self._close()
        with self._lock:
            self._chunks = []


# ---------- bridge ------------------------------------------------------

@dataclass
class ClaudeState:
    name: str = "idle"
    header2: str = "ready"


@dataclass
class Bridge:
    port: str
    baud: int = BAUD
    transcribe: Optional[Callable[[np.ndarray, int], str]] = None

    ser: serial.Serial = field(init=False)
    recorder: Recorder = field(init=False)
    state: ClaudeState = field(default_factory=ClaudeState)
    debug: bool = False
    tts_enabled: bool = True
    tts_voice: Optional[str] = None

    held_since: Optional[float] = None
    recording_started: bool = False
    cancel_record: bool = False
    last_pushed_sig: tuple = ()
    last_menu: tuple = ()
    # Persistent "info line" shown on the device status row. Survives
    # state transitions so the user always has *something* to look at.
    last_transcript: str = ""
    last_info: str = ""
    # State tracked across state_loop ticks so we can detect working→idle.
    prev_state_name: str = "idle"
    # Only speak after a push-to-talk turn, not arbitrary CLI activity.
    expect_tts: bool = False
    # Set when we've actually seen Claude enter "working" since the user's
    # last PTT — guards against speaking on a quick idle blip between the
    # user's input being pasted and the spinner appearing.
    saw_working_since_user: bool = False
    # Hard deadline so expect_tts can't stay armed forever (e.g. the user
    # denied a tool-use and Claude never produced a textual reply).
    expect_tts_deadline: float = 0.0

    _work_queue: "queue.Queue[Optional[np.ndarray]]" = field(
        default_factory=queue.Queue
    )

    def __post_init__(self) -> None:
        self.ser = serial.Serial(self.port, self.baud, timeout=0.05)
        self.recorder = Recorder()
        # ESP32 resets when the port opens; give it time to come up.
        time.sleep(2.0)
        # Transcription runs in its own thread. The serial loop must stay
        # responsive while whisper crunches — otherwise button events back
        # up and the device looks frozen.
        threading.Thread(target=self._transcribe_worker, daemon=True).start()

    # ---- serial I/O ----
    def _send(self, *parts: str) -> None:
        line = "\t".join(parts) + "\n"
        try:
            self.ser.write(line.encode("utf-8"))
            self.ser.flush()
        except Exception as exc:
            print(f"[serial] write failed: {exc}", file=sys.stderr)

    def push_state(self) -> None:
        bundle = frontmost_bundle()
        in_term = bundle in ALLOWED_BUNDLES
        app = frontmost_app_name() or "—"
        header1 = f"Claude · {app[:12]}" if in_term else app[:18]
        header2 = self.state.header2 if in_term else "(not in terminal)"
        state_name = self.state.name if in_term else "idle"
        sig = (header1, header2, state_name)
        if sig == self.last_pushed_sig:
            return
        self._send("CTX", header1, header2)
        self._send("STATE", state_name)
        self.last_pushed_sig = sig

    def menu_for_state(self) -> tuple:
        return AWAIT_MENU if self.state.name == "awaiting" else IDLE_MENU

    def push_menu(self) -> None:
        m = self.menu_for_state()
        if m != self.last_menu:
            self._send("MENU", *m)
            # Always land on the first item when the menu swaps so the
            # user isn't staring at a stale index from the previous set.
            self._send("SEL", "0")
            self.last_menu = m

    def push_info(self, text: str) -> None:
        """Update the persistent status line, skipping redundant writes."""
        text = truncate(text or "")
        if text == self.last_info:
            return
        self._send("STATUS", text)
        self.last_info = text

    def info_for_state(self, visible: str) -> str:
        st = self.state.name
        if st == "recording":
            return "REC"
        if st == "awaiting":
            return extract_approval_summary(visible) or "approve?"
        if st == "working":
            tail = extract_claude_tail(visible)
            if tail:
                return tail
            return self.last_transcript
        return self.last_transcript

    # ---- dispatch ----
    def on_tap(self, menu_idx: int) -> None:
        """Short tap → approve/deny when Claude is asking permission.

        Outside of `awaiting` there's nothing to tap — the only real
        feature is push-to-talk (hold), so we just nudge the user.
        """
        bundle = frontmost_bundle()
        if bundle not in ALLOWED_BUNDLES:
            self.push_info("not in term")
            return
        if self.state.name == "awaiting":
            # Awaiting menu is (Yes, Always, No). "No" is ESC so Claude
            # drops into "tell me what to do differently" mode. "Always"
            # types "2" to pick the "Yes, and don't ask again" option;
            # Claude's prompts use position-based numeric shortcuts, so
            # this relies on option 2 being the "always" variant.
            if menu_idx == 2:
                press_escape()
                self.push_info("denied")
            elif menu_idx == 1:
                type_char("2")
                self.push_info("always")
            else:
                press_return()
                self.push_info("approved")
            return
        self.push_info("hold to talk")

    def on_hold_start(self) -> None:
        # Shut up Claude the moment the user wants to talk again.
        say_stop()
        if self.transcribe is None:
            self.push_info("no STT")
            return
        if self.state.name == "awaiting":
            # Don't arm the mic when Claude is asking for permission;
            # the user almost certainly meant to approve, not dictate.
            return
        try:
            self.recorder.start()
        except Exception as exc:
            self.push_info("mic error")
            print(f"[mic] start failed: {exc}", file=sys.stderr)
            return
        self.recording_started = True
        self.cancel_record = False
        self._send("STATE", "recording")
        self.push_info("REC")

    def on_hold_release(self, ms_held: int) -> None:
        if not self.recording_started:
            return
        self.recording_started = False
        # Restore visual state regardless of outcome.
        if self.cancel_record:
            self.recorder.cancel()
            self.push_info("cancelled")
            self.last_pushed_sig = ()  # force a STATE re-push
            self.push_state()
            return
        audio = self.recorder.stop()
        self.last_pushed_sig = ()
        self.push_state()
        min_samples = SAMPLE_RATE * MIN_CLIP_MS // 1000
        if audio is None or len(audio) < min_samples:
            print(f"[bridge] release: held={ms_held}ms but too few samples "
                  f"({0 if audio is None else len(audio)})", file=sys.stderr)
            self.push_info("too short")
            return
        self.push_info("transcribing...")
        print(f"[bridge] release: held={ms_held}ms samples={len(audio)} "
              f"(~{len(audio)/SAMPLE_RATE:.2f}s) → queued",
              file=sys.stderr)
        # Hand off to the worker thread. Serial loop stays responsive.
        self._work_queue.put(audio)

    def _transcribe_worker(self) -> None:
        while True:
            audio = self._work_queue.get()
            if audio is None:
                continue
            try:
                self._do_transcribe_and_paste(audio)
            except Exception as exc:
                print(f"[worker] crashed: {exc!r}", file=sys.stderr)
                self.push_info("worker err")

    def _do_transcribe_and_paste(self, audio: np.ndarray) -> None:
        if self.transcribe is None:
            print("[bridge] transcribe unavailable (whisper not loaded yet?)",
                  file=sys.stderr)
            self.push_info("no STT")
            return
        try:
            t0 = time.monotonic()
            text = self.transcribe(audio, SAMPLE_RATE)
            dt = time.monotonic() - t0
            print(f"[bridge] transcribed in {dt:.2f}s: {text!r}", file=sys.stderr)
        except Exception as exc:
            print(f"[stt] failed: {exc!r}", file=sys.stderr)
            self.push_info("stt failed")
            return
        if not text:
            self.push_info("(silence)")
            return
        bundle = frontmost_bundle()
        if bundle not in ALLOWED_BUNDLES:
            # Focus moved to a non-allowlisted app between record and transcribe.
            print(f"[bridge] dropped (focus={bundle!r}): {text!r}",
                  file=sys.stderr)
            self.push_info("lost focus")
            return
        try:
            paste_via_clipboard(text)
            time.sleep(0.05)
            press_return()
            print(f"[bridge] pasted into {bundle}", file=sys.stderr)
        except Exception as exc:
            print(f"[bridge] paste failed: {exc!r}", file=sys.stderr)
            self.push_info("paste err")
            return
        # Pin the transcript as the info line. state_loop will keep
        # showing it during idle, and swap to Claude's output tail once
        # Claude starts working.
        self.last_transcript = text
        self.push_info(text)
        # Arm TTS so state_loop speaks Claude's reply once it finishes.
        # Cleared after a successful speak, or after the deadline, so we
        # never narrate CLI activity the user didn't dictate.
        if self.tts_enabled:
            self.expect_tts = True
            self.saw_working_since_user = False
            self.expect_tts_deadline = time.monotonic() + 30.0

    def on_turn(self, _delta: int) -> None:
        if self.recording_started:
            # twist-while-held = cancel; the actual cleanup happens on UP.
            self.cancel_record = True
            self.push_info("cancel")

    # ---- main loops ----
    def serial_loop(self) -> None:
        buf = b""
        while True:
            try:
                chunk = self.ser.read(64)
            except Exception as exc:
                print(f"[serial] read failed: {exc}", file=sys.stderr)
                time.sleep(0.5)
                continue
            # Hold-timeout check runs every loop iteration so it fires
            # even when the device is silent.
            if (
                self.held_since is not None
                and not self.recording_started
                and (time.monotonic() - self.held_since) * 1000 >= HOLD_MS
            ):
                self.on_hold_start()
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("utf-8", errors="ignore").strip()
                if line:
                    self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        parts = line.split("\t")
        verb = parts[0]
        if verb == "DOWN":
            self.held_since = time.monotonic()
            self.cancel_record = False
        elif verb == "UP":
            try:
                ms = int(parts[1]) if len(parts) > 1 else 0
                idx = int(parts[2]) if len(parts) > 2 else 0
            except ValueError:
                ms, idx = 0, 0
            was_recording = self.recording_started
            self.held_since = None
            if was_recording:
                self.on_hold_release(ms)
            else:
                self.on_tap(idx)
        elif verb == "TURN":
            try:
                d = int(parts[1]) if len(parts) > 1 else 0
            except ValueError:
                d = 0
            self.on_turn(d)
        elif verb == "HELLO":
            print(f"[device] {line}")
            # Drop any stale push signature so we re-send context.
            self.last_pushed_sig = ()
            self.last_menu = ()
            self.push_menu()
            self.push_state()

    def state_loop(self) -> None:
        last_dbg = ()
        while True:
            time.sleep(0.4)
            if self.recording_started:
                # Don't overwrite the "recording" state on the device.
                continue
            bundle = frontmost_bundle()
            if bundle in ALLOWED_BUNDLES:
                visible = get_visible_text(bundle)
                name, header2 = detect_claude_state(visible)
            else:
                visible = ""
                name, header2 = "idle", "(not in terminal)"
            # Speak Claude's reply once `extract_last_response` can find
            # a ⏺ block AFTER the text we pasted in. We poll every tick
            # until that's true (Claude may not have rendered yet) or the
            # deadline expires. We do NOT gate on having seen "working" —
            # if Claude responds faster than one state-loop tick, we'd
            # never see it and would stay silent forever.
            if (
                self.tts_enabled
                and self.expect_tts
                and not self.recording_started
            ):
                reply = extract_last_response(
                    visible, last_input=self.last_transcript,
                )
                if reply:
                    print(f"[bridge] reply (raw): {reply!r}", file=sys.stderr)
                    say_async(reply, voice=self.tts_voice)
                    self.expect_tts = False
                    self.saw_working_since_user = False

            # Safety: don't keep expect_tts armed forever. If we never
            # find a reply (denied tool-use, Claude exited, focus moved
            # away permanently), give up after the deadline.
            if (
                self.expect_tts
                and time.monotonic() > self.expect_tts_deadline
            ):
                print("[bridge] tts deadline expired — never found Claude's reply",
                      file=sys.stderr)
                self.expect_tts = False
                self.saw_working_since_user = False

            self.prev_state_name = name

            self.state.name = name
            self.state.header2 = header2
            self.push_state()
            self.push_menu()
            self.push_info(self.info_for_state(visible))

            if self.debug:
                sig = (bundle, name, len(visible))
                if sig != last_dbg:
                    print(
                        f"[debug] bundle={bundle!r} state={name!r} "
                        f"visible_len={len(visible)}",
                        file=sys.stderr,
                    )
                    if visible:
                        tail = "\n".join(visible.splitlines()[-12:])
                        print(
                            "[debug] tail:\n"
                            + "\n".join("  " + l for l in tail.splitlines()),
                            file=sys.stderr,
                        )
                    else:
                        print(
                            "[debug] (empty — AppleScript/AX returned nothing; "
                            "permission missing or unsupported terminal)",
                            file=sys.stderr,
                        )
                    last_dbg = sig


# ---------- entrypoint --------------------------------------------------

def find_default_port() -> Optional[str]:
    candidates = sorted(
        glob.glob("/dev/cu.usbserial*")
        + glob.glob("/dev/cu.SLAB*")
        + glob.glob("/dev/cu.wchusbserial*")
        + glob.glob("/dev/cu.usbmodem*")
    )
    return candidates[0] if candidates else None


def load_dotenv(path: str = ".env") -> None:
    """Lightweight .env loader. Existing env vars take precedence so a value
    on the command line / shell still wins over the file."""
    try:
        with open(path, encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" not in line:
                    continue
                key, _, val = line.partition("=")
                key = key.strip()
                val = val.strip().strip('"').strip("'")
                if key and key not in os.environ:
                    os.environ[key] = val
    except FileNotFoundError:
        pass
    except Exception as exc:
        print(f"[env] failed to load {path}: {exc}", file=sys.stderr)


def main() -> int:
    # Load .env before reading any environment-driven config (e.g. an
    # OpenAI key for the Whisper fallback). Lives next to this script.
    here = os.path.dirname(os.path.abspath(__file__))
    load_dotenv(os.path.join(here, ".env"))

    ap = argparse.ArgumentParser(description="minideck Claude Code bridge")
    ap.add_argument("port", nargs="?", default=None,
                    help="serial port (default: autodetect)")
    ap.add_argument("--no-stt", action="store_true",
                    help="don't load whisper (taps still work, hold becomes a no-op)")
    ap.add_argument("--no-tts", action="store_true",
                    help="don't speak Claude's reply after a push-to-talk turn")
    ap.add_argument("--voice", default=None,
                    help="macOS `say` voice name (e.g. Samantha, Alex). "
                         "Run `say -v '?'` to list.")
    ap.add_argument("--debug", action="store_true",
                    help="print frontmost bundle, detected state, and the tail "
                         "of the visible terminal text on every state-loop tick")
    args = ap.parse_args()

    port = args.port or find_default_port()
    if not port:
        print("no serial port found; pass one explicitly", file=sys.stderr)
        return 2

    print(f"[bridge] opening {port} @ {BAUD}")

    # Diagnose the AX path up front so the user knows why state detection
    # might be failing in non-AppleScript apps (Cursor, VS Code, etc.).
    if HAS_AX:
        trusted = bool(AXIsProcessTrusted()) if AXIsProcessTrusted else False
        if trusted:
            print("[ax] Accessibility permission OK — Cursor/VS Code state detection enabled")
        else:
            print("[ax] pyobjc loaded but no Accessibility permission.")
            print("[ax]   System Settings → Privacy & Security → Accessibility")
            print("[ax]   Add the binary at: " + sys.executable)
            print("[ax]   then restart the bridge.")
    else:
        print("[ax] pyobjc-framework-ApplicationServices not installed.")
        print("[ax]   Install it (the launcher does this automatically) for")
        print("[ax]   prompt detection in Cursor / VS Code / Claude desktop.")

    bridge = Bridge(
        port=port,
        debug=args.debug,
        tts_enabled=not args.no_tts,
        tts_voice=args.voice,
    )

    if not args.no_stt:
        # Load whisper in the background so the bridge is responsive
        # immediately. Holds that arrive before it's ready will report
        # "no STT" rather than blocking.
        def _load_stt() -> None:
            try:
                t0 = time.monotonic()
                bridge.transcribe = make_transcriber()
                print(f"[stt] ready (loaded in {time.monotonic()-t0:.1f}s)")
            except Exception as exc:
                print(f"[stt] {exc}", file=sys.stderr)
                print("[stt] hold-to-talk disabled; taps still work.",
                      file=sys.stderr)

        threading.Thread(target=_load_stt, daemon=True).start()

    threading.Thread(target=bridge.serial_loop, daemon=True).start()
    threading.Thread(target=bridge.state_loop, daemon=True).start()

    stop = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: stop.set())
    signal.signal(signal.SIGTERM, lambda *_: stop.set())
    stop.wait()
    print("\n[bridge] shutting down")
    return 0


if __name__ == "__main__":
    sys.exit(main())
