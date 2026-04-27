// Claude Code deck — claude3: Claude Code styled UI for ST7789 320x240.
//
// Differences vs claude2:
//   - Layout modeled on the Claude Code CLI: orange accent dot + name,
//     divider, "you said" row, "Claude said" body, menu/hint footer.
//   - New USR and BODY commands so the host can push the full user's last
//     transcript (wrapped, scrollable until BODY text arrives) and
//     Claude's reply in the body region.
//   - The encoder is purely local UI: menu selection in `awaiting`, and
//     otherwise steps through the recent history (last message / last
//     response / older turns) one item per detent. TURN is still reported
//     to the host so the bridge can use it for record-cancel etc.
//   - Host text is sanitized; every tft.print buffer is scrubbed to ASCII
//     0x20..0x7E so the default font never sees high bytes (CP437 "CJK").
//   - Main UI uses text size 2 for readability.
//
// Wire protocol (newline-delimited, \t-separated):
//
//   host -> device:
//     CTX\t<line1>\t<line2>          top bar labels ("Claude Code"/state)
//     MENU\t<label1>\t<label2>...    footer menu options
//     SEL\t<index>                   force selected index
//     STATE\t<idle|working|awaiting|recording>
//     STATUS\t<text>                 transient footer hint (overrides default)
//     USR\t<text>                    "you said …" (wrapped; dial scrolls
//                                    until BODY arrives)
//     BODY\t<text>                   Claude's reply body (wrapped, scrollable)
//     CLR                            full repaint
//
//   device -> host:
//     HELLO\tclaude-code-3            once after boot
//     DOWN                           button pressed (debounced)
//     UP\t<ms_held>\t<menu_index>    button released
//     TURN\t<delta>                  encoder rotated, signed step count
//
// Display: Adafruit_ST7789 (CS=SS, DC=4, RST=2, BL=15).
// Encoder: CLK=32, DT=33. KY-040 SW=34 (ESP32 input-only, needs external pull-up).

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <ctype.h>
#include <stdlib.h>
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

// 320x240 after setRotation(1) on a 240x320 panel.
static constexpr int16_t kDispW  = 320;
static constexpr int16_t kDispH  = 240;
static constexpr int16_t kPad    = 8;
static constexpr int16_t kInnerW = kDispW - 2 * kPad;

//   [  0.. 34]  header (title + state, size 2)
//   [ 34     ]  divider
//   [ 38..210]  live log: user transcript (grey, "> " prefix) + blank
//               line + Claude's reply (white), size 2, scrollable as one
//   [212     ]  divider
//   [220..238]  footer (menu / status, size 2)
//
// Default GFX font maps bytes >0x7E to odd CP437 glyphs ("Chinese" look).
// We scrub every printed buffer to 0x20..0x7E only.

static constexpr uint8_t kMainFontSz = 2;
static constexpr int kChrPx = 6 * kMainFontSz;
static constexpr int kBodyLineH = 8 * kMainFontSz;

static constexpr int16_t kHeaderY = 6;
static constexpr int16_t kDiv1Y   = 34;
static constexpr int16_t kBodyY   = 38;
static constexpr int16_t kBodyH   = 172;   // 38..210
static constexpr int16_t kDiv2Y   = 212;
static constexpr int16_t kFootY   = 220;

// User and Claude rows share a single left indent so wrapped continuation
// lines line up under the "> " prefix / the Claude section dot. The dot
// / prefix live in the first two glyph cells of the gutter.
static constexpr int kBodyLeftPx = kPad + 2 * kChrPx;
static constexpr int kBodyCharsPerLine =
    (kDispW - kBodyLeftPx - kPad) / kChrPx;
static constexpr int kBufCols = kBodyCharsPerLine;

static constexpr int kUserMaxLines = 32;
static constexpr int kBodyMaxLines = 64;
static constexpr int kBodyMaxRows  = kBodyH / kBodyLineH;

// ---- colors (RGB565) ------------------------------------------------------

// Approximate Claude's warm brand orange. Bright enough to read on black.
static constexpr uint16_t COL_CLAUDE   = 0xFC40;
static constexpr uint16_t COL_DOT      = 0xFD43;
static constexpr uint16_t COL_WHITE    = ST77XX_WHITE;
static constexpr uint16_t COL_GREY     = 0x8410;
static constexpr uint16_t COL_DKGREY   = 0x39E7;
static constexpr uint16_t COL_DIVIDER  = 0x2124;

// ---- state ---------------------------------------------------------------

Adafruit_ST7789 tft = Adafruit_ST7789(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

static constexpr int kMaxMenuItems = 8;
static constexpr int kMenuBufSize  = 256;
static char menuBuf[kMenuBufSize];
static const char* menuLabels[kMaxMenuItems];
static int menuCount = 0;
static int menuIndex = 0;

static char headerLine1[40] = "Claude Code";
static char headerLine2[32] = "ready";
static char stateName[16]   = "idle";

static char statusText[96]  = "";
static bool statusActive    = false;

static constexpr int kBodyMax = 1024;
static char userText[kBodyMax] = "";
static char bodyText[kBodyMax] = "";

// The live view is a single scrollable log: user transcript on top,
// blank separator row, Claude's reply below. bodyScroll is a row index
// into that combined sequence; one encoder detent == one row.
static int bodyScroll    = 0;
static int userLineCount = 0;
static int userLineStart[kUserMaxLines];
static int bodyLineCount = 0;
static int bodyLineStart[kBodyMaxLines];

volatile int16_t gEncoderSteps = 0;
volatile uint8_t gLastClk      = 0;

static bool     lastButtonDown     = false;
static uint32_t buttonDownAtMs     = 0;
static uint32_t buttonReleasedAtMs = 0;

static constexpr int kSerialBufSize = 1536;
static char serialBuf[kSerialBufSize];
static int  serialLen = 0;

// ---- util ----------------------------------------------------------------

void IRAM_ATTR onEncoderClk() {
  const int clk = digitalRead(CLK_PIN);
  if (clk != (int)gLastClk && clk == LOW) {
    if (digitalRead(DT_PIN) != clk) gEncoderSteps++;
    else                            gEncoderSteps--;
  }
  gLastClk = (uint8_t)(clk != 0);
}

static int wrapIndex(int v, int n) {
  if (n <= 0) return 0;
  return (v % n + n) % n;
}

// Rewrite `s` in place so only printable 7-bit ASCII survives. Every
// non-printable (incl. multibyte UTF-8 leads/conts like U+23CE ⏎ or the
// braille spinner) becomes a single space, runs of spaces collapse, and
// leading / trailing whitespace is trimmed. This is what keeps the
// default GFX font from rendering CP437 "Chinese-looking" glyphs for any
// stray unicode the host accidentally pushes over the wire.
static void sanitizeAscii(char* s) {
  if (!s) return;
  char* r = s;
  char* w = s;
  bool  emittedAny = false;
  bool  pendingSpace = false;
  while (*r) {
    unsigned char c = (unsigned char)*r++;
    char out;
    if (c >= 0x20 && c < 0x7F) out = (char)c;
    else if (c == '\n' || c == '\t') out = ' ';
    else                              out = ' ';
    if (out == ' ') {
      if (emittedAny) pendingSpace = true;
    } else {
      if (pendingSpace) { *w++ = ' '; pendingSpace = false; }
      *w++ = out;
      emittedAny = true;
    }
  }
  *w = '\0';
}

// Final guard before tft.print: Adafruit_GFX uses high bytes as CP437.
static void scrubForGfxPrint(char* s) {
  if (!s) return;
  for (; *s; ++s) {
    unsigned char u = (unsigned char)*s;
    if (u < 0x20 || u > 0x7e) *s = ' ';
  }
}

static uint16_t stateAccent() {
  if (strcmp(stateName, "working")   == 0) return COL_CLAUDE;
  if (strcmp(stateName, "awaiting")  == 0) return ST77XX_GREEN;
  if (strcmp(stateName, "recording") == 0) return ST77XX_RED;
  return COL_DOT;
}

// ---- draw ----------------------------------------------------------------

static inline void fillArea(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t c = ST77XX_BLACK) {
  tft.fillRect(x, y, w, h, c);
}

// Main fills use width kInnerW from x=kPad, so columns [kDispW-kPad, kDispW)
// are never touched — old scrollbar pixels would linger there. Wipe that
// strip whenever we repaint a band that can show a scroll thumb.
static inline void clearRightGutter(int16_t y, int16_t h) {
  tft.fillRect((int16_t)(kDispW - kPad), y, (int16_t)kPad, h, ST77XX_BLACK);
}

static void drawHeader() {
  fillArea(0, 0, kDispW, kDiv1Y);
  // Claude-brand dot. Small filled circle next to the title.
  tft.fillCircle(kPad + 4, kHeaderY + 8, 4, stateAccent());
  char h1[sizeof(headerLine1)];
  strncpy(h1, headerLine1, sizeof(h1) - 1);
  h1[sizeof(h1) - 1] = '\0';
  scrubForGfxPrint(h1);
  tft.setTextSize(kMainFontSz);
  tft.setTextColor(COL_WHITE);
  tft.setCursor(kPad + 14, kHeaderY);
  tft.print(h1);

  // Secondary header text (state, e.g. "working..."), right-aligned.
  if (headerLine2[0]) {
    char h2[sizeof(headerLine2)];
    strncpy(h2, headerLine2, sizeof(h2) - 1);
    h2[sizeof(h2) - 1] = '\0';
    scrubForGfxPrint(h2);
    tft.setTextSize(kMainFontSz);
    tft.setTextColor(COL_GREY);
    int w = (int)strlen(h2) * kChrPx;
    int x = kDispW - kPad - w;
    if (x < kPad) x = kPad;
    tft.setCursor(x, kHeaderY + 4);
    tft.print(h2);
  }

  tft.drawFastHLine(kPad, kDiv1Y, kInnerW, COL_DIVIDER);
}

// Soft-wrap `text` into `starts[]` (indices of row starts). Breaks on '\n'
// when it appears before the soft limit, on the last space in [i, i+wrapW)
// otherwise, and hard-breaks mid-word when there is no space. No allocs.
static void wrapText(const char* text, int wrapW,
                     int* starts, int maxStarts, int* countOut) {
  *countOut = 0;
  const int len = (int)strlen(text);
  if (len == 0 || wrapW < 4) return;
  int i = 0;
  while (i < len && *countOut < maxStarts) {
    starts[(*countOut)++] = i;
    int scanEnd = i + wrapW;
    if (scanEnd > len) scanEnd = len;
    int nl = -1;
    for (int j = i; j < scanEnd; j++) if (text[j] == '\n') { nl = j; break; }
    if (nl >= 0) { i = nl + 1; continue; }
    if (scanEnd >= len) { i = len; break; }
    int brk = -1;
    for (int j = scanEnd; j > i; j--) if (text[j] == ' ') { brk = j; break; }
    if (brk < 0) brk = scanEnd;
    i = brk;
    while (i < len && text[i] == ' ') i++;
  }
}

// Copy text[s..e) (trimmed of trailing whitespace) into a scratch buf,
// scrub to printable ASCII, then print at (x, y) with the current color.
static void drawWrapSlice(const char* text, int s, int e, int maxW,
                          int16_t x, int16_t y) {
  while (e > s && (text[e - 1] == ' ' || text[e - 1] == '\n')) e--;
  int n = e - s;
  if (n < 0) n = 0;
  if (n > maxW) n = maxW;
  char buf[kBufCols + 1];
  memcpy(buf, text + s, n);
  buf[n] = '\0';
  scrubForGfxPrint(buf);
  tft.setCursor(x, y);
  tft.print(buf);
}

// Render the combined live log: "> <user message>" (grey) + blank row +
// "<Claude reply>" (white), scrolled by bodyScroll. One detent on the dial
// moves one row. Both sections share the same left indent so wrapped
// continuation lines line up under the "> " and the section dot.
static void drawLiveBody() {
  fillArea(kPad, kBodyY - 2, kInnerW, kBodyH);
  clearRightGutter((int16_t)(kBodyY - 2), kBodyH);

  wrapText(userText, kBodyCharsPerLine,
           userLineStart, kUserMaxLines, &userLineCount);
  wrapText(bodyText, kBodyCharsPerLine,
           bodyLineStart, kBodyMaxLines, &bodyLineCount);

  const int sepRows = (userLineCount > 0 && bodyLineCount > 0) ? 1 : 0;
  const int totalRows = userLineCount + sepRows + bodyLineCount;

  tft.setTextSize(kMainFontSz);

  if (totalRows == 0) {
    const char* placeholder;
    if      (strcmp(stateName, "recording") == 0) placeholder = "listening...";
    else if (strcmp(stateName, "working")   == 0) placeholder = "Claude is thinking...";
    else if (strcmp(stateName, "awaiting")  == 0) placeholder = "Claude needs approval";
    else                                          placeholder = "(hold to talk)";
    tft.setTextColor(COL_DKGREY);
    tft.setCursor(kBodyLeftPx, kBodyY + 2);
    tft.print(placeholder);
    return;
  }

  int maxScroll = totalRows - kBodyMaxRows;
  if (maxScroll < 0) maxScroll = 0;
  if (bodyScroll < 0)         bodyScroll = 0;
  if (bodyScroll > maxScroll) bodyScroll = maxScroll;

  int rowsToDraw = totalRows - bodyScroll;
  if (rowsToDraw > kBodyMaxRows) rowsToDraw = kBodyMaxRows;

  const int userLen = (int)strlen(userText);
  const int bodyLen = (int)strlen(bodyText);

  for (int r = 0; r < rowsToDraw; r++) {
    const int gi   = r + bodyScroll;
    const int16_t y = (int16_t)(kBodyY + r * kBodyLineH);

    if (gi < userLineCount) {
      // User section (grey). First visible line carries the "> " gutter.
      if (gi == 0) {
        tft.setTextColor(COL_DKGREY);
        tft.setCursor(kPad, y);
        tft.print("> ");
      }
      const int s = userLineStart[gi];
      const int e = (gi + 1 < userLineCount) ? userLineStart[gi + 1] : userLen;
      tft.setTextColor(COL_GREY);
      drawWrapSlice(userText, s, e, kBodyCharsPerLine,
                    (int16_t)kBodyLeftPx, y);
    } else if (gi < userLineCount + sepRows) {
      // Intentionally blank separator row between user and Claude.
    } else {
      // Claude section (white). First visible line carries the brand dot.
      const int bi = gi - userLineCount - sepRows;
      if (bi == 0) {
        tft.fillCircle(kPad + 4, y + kBodyLineH / 2, 3, COL_CLAUDE);
      }
      const int s = bodyLineStart[bi];
      const int e = (bi + 1 < bodyLineCount) ? bodyLineStart[bi + 1] : bodyLen;
      tft.setTextColor(COL_WHITE);
      drawWrapSlice(bodyText, s, e, kBodyCharsPerLine,
                    (int16_t)kBodyLeftPx, y);
    }
  }

  if (totalRows > kBodyMaxRows) {
    const int trackH = kBodyH - 4;
    const int trackY = kBodyY;
    int thumbH = (trackH * kBodyMaxRows) / totalRows;
    if (thumbH < 8) thumbH = 8;
    const int denom  = maxScroll > 0 ? maxScroll : 1;
    const int thumbY = trackY + (trackH - thumbH) * bodyScroll / denom;
    const int barX   = kDispW - kPad - 4;
    tft.drawFastVLine(barX, trackY, trackH, COL_DIVIDER);
    tft.fillRect(barX + 1, thumbY, 3, thumbH, COL_CLAUDE);
  }
}

static void drawFooter() {
  fillArea(0, kDiv2Y, kDispW, kDispH - kDiv2Y);
  tft.drawFastHLine(kPad, kDiv2Y, kInnerW, COL_DIVIDER);

  tft.setTextSize(kMainFontSz);
  int leftEnd = kPad;
  if (menuCount == 0) {
    tft.setTextColor(COL_DKGREY);
    tft.setCursor(kPad, kFootY + 2);
    tft.print("(no menu)");
    leftEnd = kPad + 9 * kChrPx;
  } else {
    char lblb[80];
    strncpy(lblb, menuLabels[menuIndex], sizeof(lblb) - 1);
    lblb[sizeof(lblb) - 1] = '\0';
    scrubForGfxPrint(lblb);
    tft.setTextColor(stateAccent());
    tft.setCursor(kPad, kFootY + 2);
    tft.print("[ ");
    tft.print(lblb);
    tft.print(" ]");
    int usedChars = (int)strlen(lblb) + 4;
    leftEnd = kPad + usedChars * kChrPx;
    if (menuCount > 1) {
      char chip[16];
      snprintf(chip, sizeof(chip), "%d/%d", menuIndex + 1, menuCount);
      scrubForGfxPrint(chip);
      tft.setTextColor(COL_DKGREY);
      tft.setCursor(leftEnd + 4, kFootY + 2);
      tft.print(chip);
      leftEnd += 4 + (int)strlen(chip) * kChrPx;
    }
  }

  // Right side: only the persistent status line, if any. Help "tips"
  // (hold to talk / turn to scroll / ...) are intentionally suppressed.
  if (!statusActive) return;
  static char hintDraw[sizeof(statusText)];
  strncpy(hintDraw, statusText, sizeof(hintDraw) - 1);
  hintDraw[sizeof(hintDraw) - 1] = '\0';
  scrubForGfxPrint(hintDraw);
  const char* hint = hintDraw;

  int hw = (int)strlen(hint) * kChrPx;
  int hx = kDispW - kPad - hw;
  if (hx < leftEnd + 8) {
    hx = leftEnd + 8;
    int avail = kDispW - kPad - hx;
    int maxChars = avail / kChrPx;
    if (maxChars < 0) maxChars = 0;
    if ((int)strlen(hint) > maxChars) {
      static char clipped[sizeof(statusText)];
      int n = maxChars;
      if (n > (int)sizeof(clipped) - 1) n = (int)sizeof(clipped) - 1;
      if (n > 3) {
        memcpy(clipped, hint, n);
        clipped[n - 1] = '.';
        clipped[n - 2] = '.';
        clipped[n - 3] = '.';
        clipped[n] = '\0';
        scrubForGfxPrint(clipped);
        hint = clipped;
      }
    }
  }
  tft.setTextColor(stateAccent());
  tft.setCursor(hx, kFootY + 2);
  tft.print(hint);
}

static void redrawAll() {
  tft.fillScreen(ST77XX_BLACK);
  drawHeader();
  drawLiveBody();
  drawFooter();
}

// ---- protocol ------------------------------------------------------------

static void setStatus(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    if (!statusActive) return;
    statusActive = false;
    statusText[0] = '\0';
    drawFooter();
    return;
  }
  strncpy(statusText, text, sizeof(statusText) - 1);
  statusText[sizeof(statusText) - 1] = '\0';
  sanitizeAscii(statusText);
  statusActive = (statusText[0] != '\0');
  drawFooter();
}

static void parseMenuLine(char* args) {
  menuCount = 0;
  size_t bufUsed = 0;
  char* save = nullptr;
  char* tok = strtok_r(args, "\t", &save);
  while (tok && menuCount < kMaxMenuItems) {
    sanitizeAscii(tok);
    size_t l = strlen(tok);
    if (l == 0) { tok = strtok_r(nullptr, "\t", &save); continue; }
    if (bufUsed + l + 1 > kMenuBufSize) break;
    memcpy(menuBuf + bufUsed, tok, l + 1);
    menuLabels[menuCount++] = menuBuf + bufUsed;
    bufUsed += l + 1;
    tok = strtok_r(nullptr, "\t", &save);
  }
  if (menuIndex >= menuCount) menuIndex = 0;
  drawFooter();
}

static void parseCtxLine(char* args) {
  char* tab = strchr(args, '\t');
  if (tab) {
    *tab = '\0';
    strncpy(headerLine1, args, sizeof(headerLine1) - 1);
    headerLine1[sizeof(headerLine1) - 1] = '\0';
    strncpy(headerLine2, tab + 1, sizeof(headerLine2) - 1);
    headerLine2[sizeof(headerLine2) - 1] = '\0';
  } else {
    strncpy(headerLine1, args, sizeof(headerLine1) - 1);
    headerLine1[sizeof(headerLine1) - 1] = '\0';
    headerLine2[0] = '\0';
  }
  sanitizeAscii(headerLine1);
  sanitizeAscii(headerLine2);
  drawHeader();
}

static void setUser(const char* text) {
  const char* src = text ? text : "";
  if (strcmp(userText, src) == 0) return;  // dedupe identical replays
  strncpy(userText, src, sizeof(userText) - 1);
  userText[sizeof(userText) - 1] = '\0';
  sanitizeAscii(userText);
  // Fresh turn: park scroll at the top so the user sees their own
  // message first; they can dial down to see Claude as it arrives.
  bodyScroll = 0;
  drawLiveBody();
}

static void setBody(const char* text) {
  const bool wasEmpty = (bodyText[0] == '\0');
  strncpy(bodyText, text ? text : "", sizeof(bodyText) - 1);
  bodyText[sizeof(bodyText) - 1] = '\0';
  sanitizeAscii(bodyText);
  // First reply content for this turn: auto-scroll to the start of
  // Claude's section so the answer is immediately visible. User can
  // dial up to re-read their own message. Subsequent streaming updates
  // preserve whatever scroll position the user is currently reading.
  if (wasEmpty && bodyText[0] != '\0' && userText[0] != '\0') {
    int ulc = 0;
    int scratch[kUserMaxLines];
    wrapText(userText, kBodyCharsPerLine, scratch, kUserMaxLines, &ulc);
    bodyScroll = ulc + 1;  // user rows + separator
  }
  drawLiveBody();
}

static void handleLine(char* line) {
  if (line[0] == '\0') return;
  char* tab = strchr(line, '\t');
  char* args = nullptr;
  if (tab) { *tab = '\0'; args = tab + 1; }

  if (strcmp(line, "CTX") == 0 && args) {
    parseCtxLine(args);
  } else if (strcmp(line, "MENU") == 0 && args) {
    parseMenuLine(args);
  } else if (strcmp(line, "SEL") == 0 && args) {
    int v = atoi(args);
    if (menuCount > 0) {
      menuIndex = wrapIndex(v, menuCount);
      drawFooter();
    }
  } else if (strcmp(line, "STATE") == 0 && args) {
    strncpy(stateName, args, sizeof(stateName) - 1);
    stateName[sizeof(stateName) - 1] = '\0';
    sanitizeAscii(stateName);
    drawHeader();
    // Empty-view placeholder depends on state ("listening..." /
    // "Claude is thinking..." / ...); cheap to repaint.
    if (userLineCount == 0 && bodyLineCount == 0) drawLiveBody();
    drawFooter();
  } else if (strcmp(line, "STATUS") == 0) {
    setStatus(args ? args : "");
  } else if (strcmp(line, "USR") == 0) {
    setUser(args ? args : "");
  } else if (strcmp(line, "BODY") == 0) {
    setBody(args ? args : "");
  } else if (strcmp(line, "CLR") == 0) {
    redrawAll();
  }
}

static void pollSerial() {
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      serialBuf[serialLen] = '\0';
      handleLine(serialBuf);
      serialLen = 0;
      continue;
    }
    if (serialLen < kSerialBufSize - 1) {
      serialBuf[serialLen++] = c;
    } else {
      // Overflow: drop the in-progress line entirely rather than render
      // a truncated frame. The host can resend on CLR.
      serialLen = 0;
    }
  }
}

static void drainEncoder() {
  int16_t steps;
  noInterrupts();
  steps = gEncoderSteps;
  gEncoderSteps = 0;
  interrupts();
  if (steps == 0) return;

  // Local-only UI: navigate approval menu in `awaiting`, otherwise
  // scroll the unified live log (user message + Claude reply). One
  // detent == one wrapped row; drawLiveBody clamps to bounds.
  if (strcmp(stateName, "awaiting") == 0 && menuCount > 0) {
    menuIndex = wrapIndex(menuIndex + (int)steps, menuCount);
    drawFooter();
  } else if (userLineCount > 0 || bodyLineCount > 0) {
    bodyScroll += (int)steps;
    drawLiveBody();
  }

  // Still report to the host — the bridge uses TURN for things like
  // "twist-to-cancel-recording".
  Serial.print("TURN\t");
  Serial.println((int)steps);
}

static void pollButton() {
  const bool down = (digitalRead(SW_PIN) == LOW);
  const uint32_t now = millis();
  if (down && !lastButtonDown) {
    if (now - buttonReleasedAtMs >= kButtonDebounceMs) {
      buttonDownAtMs = now;
      Serial.println("DOWN");
      lastButtonDown = true;
    }
  } else if (!down && lastButtonDown) {
    if (now - buttonDownAtMs >= kButtonDebounceMs) {
      const uint32_t held = now - buttonDownAtMs;
      Serial.print("UP\t");
      Serial.print((unsigned long)held);
      Serial.print("\t");
      Serial.println(menuIndex);
      buttonReleasedAtMs = now;
      lastButtonDown = false;
    }
  }
}

// ---- setup/loop ----------------------------------------------------------

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

  lastButtonDown     = (digitalRead(SW_PIN) == LOW);
  buttonReleasedAtMs = millis();
  buttonDownAtMs     = millis();

  redrawAll();

  Serial.println("HELLO\tclaude-code-3");
}

void loop() {
  pollSerial();
  drainEncoder();
  pollButton();
  delay(2);
}
