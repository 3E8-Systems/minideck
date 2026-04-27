// minideck — timer: standalone countdown timer for ST7789 320x240 + KY-040.
//
// Local-only app (no host bridge). UX:
//   - In idle, the dial sets the target time. Step size adapts to the
//     current value so you can nudge 5 s at the low end or stride 5 min
//     at the high end without ever dialing for forever:
//         <  1 min   ->  5 s per detent
//         < 10 min   -> 30 s
//         <  1 h     ->  1 min
//         >=  1 h    ->  5 min
//     When stepping down across a threshold we use the smaller step
//     size so you never over-shoot below the boundary in a single click.
//   - Short press: start  / pause / resume / dismiss (on done).
//   - Long press (>=600 ms): reset to idle (keeps the last set time).
//     In idle with a value loaded, long press clears it back to 0.
//   - When the countdown reaches 0 the display flashes until you press.
//
// Display: Adafruit_ST7789 (CS=SS, DC=4, RST=2, BL=15).
// Encoder: CLK=32, DT=33. KY-040 SW=34 (ESP32 input-only, needs external
// pull-up on the switch line).
//
// Style and pinout intentionally mirror claude3.ino so any rig that runs
// that sketch will drop-in run this one.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// ---- pins -----------------------------------------------------------------

const int PIN_LCD_CS  = SS;
const int PIN_LCD_DC  = 4;
const int PIN_LCD_RST = 2;
const int PIN_LCD_BL  = 15;

#define CLK_PIN 32
#define DT_PIN  33
#define SW_PIN  34

// ---- layout ---------------------------------------------------------------

static constexpr uint32_t kButtonDebounceMs = 35;
static constexpr uint32_t kLongPressMs      = 600;

// 320x240 after setRotation(1) on a 240x320 panel.
static constexpr int16_t kDispW  = 320;
static constexpr int16_t kDispH  = 240;
static constexpr int16_t kPad    = 8;
static constexpr int16_t kInnerW = kDispW - 2 * kPad;

//   [  0.. 34]  header (title, state)
//   [ 34     ]  divider
//   [ 36..156]  time readout (centered, size 6 or 8)
//   [168..182]  progress bar
//   [212     ]  divider
//   [218..238]  footer (primary action + hint)
static constexpr int16_t kHeaderY     = 6;
static constexpr int16_t kDiv1Y       = 34;
static constexpr int16_t kTimeAreaY   = 36;
static constexpr int16_t kTimeAreaH   = 120;
static constexpr int16_t kBarY        = 168;
static constexpr int16_t kBarH        = 14;
static constexpr int16_t kDiv2Y       = 212;
static constexpr int16_t kFootY       = 220;

// Default GFX font is 6x8 per glyph; everything we scale with setTextSize.
static constexpr int     kGfxChrW = 6;
static constexpr int     kGfxChrH = 8;
static constexpr uint8_t kHeadSz  = 2;   // header / state label
static constexpr uint8_t kFootSz  = 2;   // footer
static constexpr uint8_t kBigSz   = 8;   // MM:SS at full size (48x64 glyphs)
static constexpr uint8_t kMedSz   = 6;   // H:MM:SS fits at 6 (36x48 glyphs)

// ---- colors (RGB565) ------------------------------------------------------

static constexpr uint16_t COL_ACCENT  = 0xFC40;   // warm orange (claude vibe)
static constexpr uint16_t COL_DOT     = 0xFD43;
static constexpr uint16_t COL_WHITE   = ST77XX_WHITE;
static constexpr uint16_t COL_GREY    = 0x8410;
static constexpr uint16_t COL_DKGREY  = 0x39E7;
static constexpr uint16_t COL_DIVIDER = 0x2124;
static constexpr uint16_t COL_GREEN   = 0x07E0;
static constexpr uint16_t COL_RED     = 0xF800;

// ---- state ----------------------------------------------------------------

Adafruit_ST7789 tft = Adafruit_ST7789(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

enum TimerState : uint8_t {
  S_IDLE = 0,
  S_RUNNING,
  S_PAUSED,
  S_DONE,
};

// All time math lives in milliseconds on `millis()` so it stays monotonic
// across pauses. `setSec` is the last value the user dialled and we keep
// it through transitions so "reset" returns to the user's configured time.
static TimerState state    = S_IDLE;
static uint32_t   setSec   = 0;
static uint32_t   remainMs = 0;    // valid when paused; recomputed live while running
static uint32_t   endMs    = 0;    // `millis()` target; valid only when running
static uint32_t   totalMs  = 0;    // denominator for progress bar; captured at start

// Max 24 h keeps everything inside int32 arithmetic comfortably.
static constexpr uint32_t kMaxSec = 24UL * 3600UL;

// Render caches so the running tick can short-circuit when nothing on
// screen actually changed. Seeded to sentinel values that force the
// first paint to be a full one.
static char    shownTime[12] = "";
static uint8_t shownTimeSz   = 0;
static int     shownBarPx    = -1;
static uint8_t shownState    = 255;
static bool    shownFlash    = false;

// encoder
volatile int16_t gEncoderSteps = 0;
volatile uint8_t gLastClk      = 0;

// button
static bool     btnDown         = false;
static uint32_t btnDownAtMs     = 0;
static uint32_t btnReleasedAtMs = 0;
static bool     longFired       = false;  // suppress the trailing short-press

// ---- utils ----------------------------------------------------------------

void IRAM_ATTR onEncoderClk() {
  const int clk = digitalRead(CLK_PIN);
  if (clk != (int)gLastClk && clk == LOW) {
    if (digitalRead(DT_PIN) != clk) gEncoderSteps++;
    else                            gEncoderSteps--;
  }
  gLastClk = (uint8_t)(clk != 0);
}

static uint32_t stepFor(uint32_t sec) {
  if (sec < 60)   return 5;
  if (sec < 600)  return 30;
  if (sec < 3600) return 60;
  return 300;
}

static uint32_t clampSec(int64_t s) {
  if (s < 0) return 0;
  if ((uint64_t)s > (uint64_t)kMaxSec) return kMaxSec;
  return (uint32_t)s;
}

// Fills `out` with the HH:MM:SS or MM:SS form and tells the caller which
// font size to use. H:MM:SS is 7 glyphs -> fits at size 6 (252 px wide);
// MM:SS is 5 glyphs and gets to breathe at size 8 (240 px wide).
static void formatTime(uint32_t totalSec, char* out, size_t outSz,
                       uint8_t* outSize) {
  const uint32_t h = totalSec / 3600;
  const uint32_t m = (totalSec % 3600) / 60;
  const uint32_t s = totalSec % 60;
  if (h > 0) {
    snprintf(out, outSz, "%lu:%02lu:%02lu",
             (unsigned long)h, (unsigned long)m, (unsigned long)s);
    *outSize = kMedSz;
  } else {
    snprintf(out, outSz, "%02lu:%02lu",
             (unsigned long)m, (unsigned long)s);
    *outSize = kBigSz;
  }
}

// millis() math is unsigned modular; cast to signed to detect the target
// being in the past. Our max remaining (24 h ≈ 8.6e7 ms) stays well inside
// int32 range so this is safe.
static uint32_t currentRemainMs() {
  if (state == S_RUNNING) {
    const uint32_t now = millis();
    const int32_t  d   = (int32_t)(endMs - now);
    return d > 0 ? (uint32_t)d : 0;
  }
  return remainMs;
}

// We show the ceiling of remaining seconds so "00:01" stays visible for
// its full last second rather than flashing to "00:00" half a second early.
static uint32_t currentRemainSec() {
  switch (state) {
    case S_IDLE: return setSec;
    case S_DONE: return 0;
    default: {
      const uint32_t ms = currentRemainMs();
      return (ms + 999U) / 1000U;
    }
  }
}

// ---- drawing --------------------------------------------------------------

static const char* stateLabel() {
  switch (state) {
    case S_IDLE:    return setSec > 0 ? "ready" : "set time";
    case S_RUNNING: return "running";
    case S_PAUSED:  return "paused";
    case S_DONE:    return "done!";
  }
  return "";
}

static uint16_t stateAccent() {
  switch (state) {
    case S_RUNNING: return COL_GREEN;
    case S_PAUSED:  return COL_ACCENT;
    case S_DONE:    return COL_RED;
    default:        return COL_DOT;
  }
}

static void drawHeader() {
  tft.fillRect(0, 0, kDispW, kDiv1Y, ST77XX_BLACK);
  tft.fillCircle(kPad + 4, kHeaderY + 8, 4, stateAccent());

  tft.setTextSize(kHeadSz);
  tft.setTextColor(COL_WHITE);
  tft.setCursor(kPad + 14, kHeaderY);
  tft.print("Timer");

  const char* lbl = stateLabel();
  tft.setTextColor(COL_GREY);
  const int w = (int)strlen(lbl) * kGfxChrW * kHeadSz;
  int x = kDispW - kPad - w;
  if (x < kPad) x = kPad;
  tft.setCursor(x, kHeaderY + 4);
  tft.print(lbl);

  tft.drawFastHLine(kPad, kDiv1Y, kInnerW, COL_DIVIDER);
}

static void drawTime(bool flashOff) {
  char    buf[12];
  uint8_t sz;
  formatTime(currentRemainSec(), buf, sizeof(buf), &sz);

  // Short-circuit: if nothing visible in this band actually changed we can
  // skip the expensive fillRect + glyph draw. shownState covers color
  // changes on state transition; shownFlash covers the done-state blink.
  const bool sameShape = (sz == shownTimeSz) && (state == shownState);
  if (sameShape && strcmp(buf, shownTime) == 0 && flashOff == shownFlash) {
    return;
  }

  tft.fillRect(0, kTimeAreaY, kDispW, kTimeAreaH, ST77XX_BLACK);

  const int glyphW = kGfxChrW * sz;
  const int glyphH = kGfxChrH * sz;
  const int textW  = (int)strlen(buf) * glyphW;
  const int x      = (kDispW - textW) / 2;
  const int y      = kTimeAreaY + (kTimeAreaH - glyphH) / 2;

  uint16_t col;
  if (state == S_DONE) {
    col = flashOff ? COL_DKGREY : COL_RED;
  } else if (state == S_IDLE && setSec == 0) {
    col = COL_DKGREY;
  } else if (state == S_PAUSED) {
    col = COL_ACCENT;
  } else {
    col = COL_WHITE;
  }

  tft.setTextSize(sz);
  tft.setTextColor(col);
  tft.setCursor(x, y);
  tft.print(buf);

  strncpy(shownTime, buf, sizeof(shownTime) - 1);
  shownTime[sizeof(shownTime) - 1] = '\0';
  shownTimeSz = sz;
  shownState  = (uint8_t)state;
  shownFlash  = flashOff;
}

static void drawBar() {
  int px;
  if (state == S_DONE) {
    px = kInnerW;
  } else if (state == S_IDLE || totalMs == 0) {
    px = 0;
  } else {
    uint32_t rem = currentRemainMs();
    if (rem > totalMs) rem = totalMs;
    const uint32_t elapsed = totalMs - rem;
    // 64-bit multiply guards against overflow for hour-scale timers.
    const uint64_t scaled = (uint64_t)elapsed * (uint64_t)kInnerW /
                            (uint64_t)totalMs;
    px = (int)scaled;
    if (px < 0)        px = 0;
    if (px > kInnerW)  px = kInnerW;
  }

  if (px == shownBarPx) return;

  tft.fillRect(kPad, kBarY, kInnerW, kBarH, ST77XX_BLACK);
  tft.drawRect(kPad, kBarY, kInnerW, kBarH, COL_DIVIDER);
  const uint16_t fill = (state == S_IDLE) ? COL_DKGREY : stateAccent();
  if (px > 2) {
    tft.fillRect(kPad + 1, kBarY + 1, px - 2, kBarH - 2, fill);
  }
  shownBarPx = px;
}

static void drawFooter() {
  tft.fillRect(0, kDiv2Y, kDispW, kDispH - kDiv2Y, ST77XX_BLACK);
  tft.drawFastHLine(kPad, kDiv2Y, kInnerW, COL_DIVIDER);

  const char* primary;
  const char* hint = "";
  switch (state) {
    case S_IDLE:
      primary = setSec > 0 ? "Start" : "Turn dial";
      hint    = setSec > 0 ? "hold=clear" : "";
      break;
    case S_RUNNING:
      primary = "Pause";
      hint    = "hold=reset";
      break;
    case S_PAUSED:
      primary = "Resume";
      hint    = "hold=reset";
      break;
    case S_DONE:
      primary = "Dismiss";
      hint    = "";
      break;
    default:
      primary = "";
      break;
  }

  tft.setTextSize(kFootSz);
  tft.setTextColor(stateAccent());
  tft.setCursor(kPad, kFootY + 2);
  tft.print("[ ");
  tft.print(primary);
  tft.print(" ]");

  if (hint[0]) {
    tft.setTextColor(COL_DKGREY);
    const int w = (int)strlen(hint) * kGfxChrW * kFootSz;
    int x = kDispW - kPad - w;
    if (x < kPad) x = kPad;
    tft.setCursor(x, kFootY + 2);
    tft.print(hint);
  }
}

static void redrawAll() {
  tft.fillScreen(ST77XX_BLACK);
  // Force every cached region to repaint by clobbering the shadows.
  shownTime[0] = '\0';
  shownTimeSz  = 0;
  shownBarPx   = -1;
  shownState   = 255;
  shownFlash   = false;
  drawHeader();
  drawTime(false);
  drawBar();
  drawFooter();
}

// ---- transitions ----------------------------------------------------------

static void enterIdle() {
  state    = S_IDLE;
  endMs    = 0;
  remainMs = (uint32_t)setSec * 1000UL;
  totalMs  = 0;
  drawHeader();
  drawTime(false);
  drawBar();
  drawFooter();
}

static void enterRunning() {
  // Guard: nothing to count down.
  if (state == S_IDLE && setSec == 0) return;

  if (state == S_IDLE) {
    remainMs = (uint32_t)setSec * 1000UL;
    totalMs  = remainMs;  // captured once so the bar scales against the
                          // original target across pause/resume cycles
  }
  // Resuming from paused keeps totalMs intact; remainMs already reflects
  // the frozen tail we're about to re-anchor to `now`.
  if (remainMs == 0) return;

  endMs = millis() + remainMs;
  state = S_RUNNING;
  drawHeader();
  drawFooter();
  drawTime(false);
  drawBar();
}

static void enterPaused() {
  remainMs = currentRemainMs();
  endMs    = 0;
  state    = S_PAUSED;
  drawHeader();
  drawFooter();
  drawTime(false);
}

static void enterDone() {
  state    = S_DONE;
  endMs    = 0;
  remainMs = 0;
  drawHeader();
  drawFooter();
  drawTime(false);
  drawBar();
}

// ---- input handlers -------------------------------------------------------

static void onShortPress() {
  switch (state) {
    case S_IDLE:    if (setSec > 0) enterRunning(); break;
    case S_RUNNING: enterPaused();                  break;
    case S_PAUSED:  enterRunning();                 break;
    case S_DONE:    enterIdle();                    break;
  }
}

static void onLongPress() {
  switch (state) {
    case S_RUNNING:
    case S_PAUSED:
    case S_DONE:
      enterIdle();  // keep setSec so the dial value persists
      break;
    case S_IDLE:
      if (setSec != 0) {
        setSec   = 0;
        remainMs = 0;
        drawHeader();
        drawTime(false);
        drawFooter();
      }
      break;
  }
}

// Process encoder detents one at a time so stepFor() can adapt as we
// cross thresholds mid-spin. Going down across a threshold we deliberately
// use the smaller side's step so a single click can never overshoot below
// the boundary (e.g. 60 -> 55, not 60 -> 30).
static void handleEncoder(int16_t steps) {
  if (state != S_IDLE) return;

  const int32_t dir = steps > 0 ? 1 : -1;
  int32_t       n   = steps > 0 ? steps : -steps;
  int64_t       sec = (int64_t)setSec;

  for (int32_t i = 0; i < n; i++) {
    uint32_t step = stepFor((uint32_t)(sec > 0 ? sec : 0));
    if (dir < 0 && sec > 0) {
      const uint32_t downStep =
          stepFor((uint32_t)((sec - 1) > 0 ? sec - 1 : 0));
      if (downStep < step) step = downStep;
    }
    sec += (int64_t)dir * (int64_t)step;
    if (sec <= 0) { sec = 0; break; }
    if (sec >= (int64_t)kMaxSec) { sec = kMaxSec; break; }
  }

  const uint32_t newSec = clampSec(sec);
  if (newSec == setSec) return;
  const bool wasZero = (setSec == 0);
  setSec   = newSec;
  remainMs = (uint32_t)setSec * 1000UL;
  const bool isZero = (setSec == 0);
  drawTime(false);
  // Only the zero boundary flips the header label ("set time" <-> "ready")
  // and the footer primary ("Turn dial" <-> "Start"). Skip the repaint on
  // every detent so fast spins don't flicker the chrome.
  if (wasZero != isZero) {
    drawHeader();
    drawFooter();
  }
}

static void drainEncoder() {
  int16_t steps;
  noInterrupts();
  steps = gEncoderSteps;
  gEncoderSteps = 0;
  interrupts();
  if (steps != 0) handleEncoder(steps);
}

// Edge-triggered button polling with a symmetric debounce on both press
// and release. We fire onLongPress() while the button is still held so
// the reset feels instantaneous, and set `longFired` to suppress the
// trailing short-press on release.
static void pollButton() {
  const bool     down = (digitalRead(SW_PIN) == LOW);
  const uint32_t now  = millis();

  if (down && !btnDown) {
    if (now - btnReleasedAtMs >= kButtonDebounceMs) {
      btnDownAtMs = now;
      btnDown     = true;
      longFired   = false;
    }
  } else if (!down && btnDown) {
    if (now - btnDownAtMs >= kButtonDebounceMs) {
      const uint32_t held = now - btnDownAtMs;
      btnReleasedAtMs = now;
      btnDown         = false;
      if (!longFired && held < kLongPressMs) {
        onShortPress();
      }
    }
  } else if (down && btnDown && !longFired) {
    if (now - btnDownAtMs >= kLongPressMs) {
      longFired = true;
      onLongPress();
    }
  }
}

static void tickRunning() {
  if (state != S_RUNNING) return;
  const uint32_t now = millis();
  if ((int32_t)(endMs - now) <= 0) {
    enterDone();
    return;
  }
  drawTime(false);
  drawBar();
}

// 1 Hz blink (500 ms on / 500 ms off) driven off millis() so we don't
// need a dedicated timer — tick()'s cache skips the draw when the phase
// hasn't flipped since the last call.
static void tickDone() {
  if (state != S_DONE) return;
  const bool off = ((millis() / 500UL) & 1UL) != 0;
  drawTime(off);
}

// ---- setup / loop ---------------------------------------------------------

void setup() {
  Serial.begin(115200);

  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  tft.init(240, 320);
  tft.setRotation(1);
  tft.setTextWrap(false);
  tft.fillScreen(ST77XX_BLACK);

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN,  INPUT_PULLUP);
#if SW_PIN >= 34 && SW_PIN <= 39
  pinMode(SW_PIN, INPUT);        // ESP32 GPIO34-39 have no internal pull-ups
#else
  pinMode(SW_PIN, INPUT_PULLUP);
#endif

  gLastClk = (uint8_t)(digitalRead(CLK_PIN) != 0);
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), onEncoderClk, CHANGE);

  // If the button happens to be held at boot we latch the "already down"
  // state so no spurious press fires on the way up.
  btnDown         = (digitalRead(SW_PIN) == LOW);
  btnReleasedAtMs = millis();
  btnDownAtMs     = millis();
  longFired       = btnDown;

  redrawAll();

  Serial.println("HELLO\tminideck-timer");
}

void loop() {
  drainEncoder();
  pollButton();
  tickRunning();
  tickDone();
  delay(5);
}
