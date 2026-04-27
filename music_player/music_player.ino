// minideck — music_player: Claude-code styled UI, repurposed for controlling
// the Mac's music player. Based on claude3.ino (same ST7789 320x240 + KY-040).
//
// Encoder UX:
//   CW one detent  -> NEXT  track
//   CCW one detent -> PREV  track
//   Push button    -> PLAY_PAUSE
//
// Wire protocol (newline-delimited, \t-separated):
//
//   host -> device:
//     META\t<artist>\t<title>\t<album>   text metadata (sanitized to ASCII)
//     STATE\t<playing|paused|stopped|none>
//     ART\t<w>\t<h>\t<nchunks>\t<hash>   begin artwork transfer (RGB565)
//     A\t<idx>\t<base64>                 one chunk of RGB565 bytes
//     ARTEND\t<hash>                     finalize; draw if hash matches
//     ARTCLR                             no artwork available
//     CLR                                full repaint request
//
//   device -> host:
//     HELLO\tminideck-music              once after boot
//     NEXT                               CW detent
//     PREV                               CCW detent
//     PLAY_PAUSE                         short button press
//
// Display: Adafruit_ST7789 (CS=SS, DC=4, RST=2, BL=15).
// Encoder: CLK=32, DT=33. KY-040 SW=34 (ESP32 input-only — external pull-up).

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
static constexpr uint32_t kButtonLongMs     = 600;  // ignore very long holds

// 320x240 after setRotation(1) on a 240x320 panel.
static constexpr int16_t kDispW  = 320;
static constexpr int16_t kDispH  = 240;
static constexpr int16_t kPad    = 8;
static constexpr int16_t kInnerW = kDispW - 2 * kPad;

// Header (top) / footer (bottom) match claude3 so the visual language is the
// same. The body splits into album-art on the left and metadata on the right.
static constexpr int16_t kHeaderY = 6;
static constexpr int16_t kDiv1Y   = 34;
static constexpr int16_t kBodyY   = 42;
static constexpr int16_t kDiv2Y   = 212;
static constexpr int16_t kFootY   = 220;

// Album art area. Square, sized as large as possible while leaving a readable
// metadata column to the right (~152 px = 12 glyphs at size 2). Going larger
// pushes the RGB565 transfer time past ~4s at 115200 baud.
static constexpr int16_t kArtW = 140;
static constexpr int16_t kArtH = 140;
static constexpr int16_t kArtX = kPad;
static constexpr int16_t kArtY = kBodyY + 4;            // 46..186

// Metadata column to the right of the artwork. Holds Artist / Title / Album
// stacked, so the UI uses the full vertical band alongside the artwork.
static constexpr int16_t kMetaX     = kArtX + kArtW + 12;  // 160
static constexpr int16_t kMetaRight = kDispW - kPad;       // 312
static constexpr int16_t kMetaW     = kMetaRight - kMetaX; // 152

static constexpr uint8_t kSmallFontSz = 1;
static constexpr uint8_t kBigFontSz   = 2;
static constexpr int kSmallChrPx = 6 * kSmallFontSz;
static constexpr int kBigChrPx   = 6 * kBigFontSz;
static constexpr int kSmallLineH = 8 * kSmallFontSz + 2;
static constexpr int kBigLineH   = 8 * kBigFontSz + 2;

static constexpr int kMetaSmallCols = (kMetaW) / kSmallChrPx;
static constexpr int kMetaBigCols   = (kMetaW) / kBigChrPx;

// ---- colors (RGB565) ------------------------------------------------------

static constexpr uint16_t COL_CLAUDE   = 0xFC40;
static constexpr uint16_t COL_DOT      = 0xFD43;
static constexpr uint16_t COL_WHITE    = ST77XX_WHITE;
static constexpr uint16_t COL_GREY     = 0x8410;
static constexpr uint16_t COL_DKGREY   = 0x39E7;
static constexpr uint16_t COL_DIVIDER  = 0x2124;
static constexpr uint16_t COL_ACCENT   = 0xFC40;  // title/highlight

// ---- state ---------------------------------------------------------------

Adafruit_ST7789 tft = Adafruit_ST7789(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

// Metadata buffers. Kept larger than the display can show so the host doesn't
// need to truncate perfectly — the draw path clips/wraps.
static char metaArtist[96] = "";
static char metaTitle[128] = "";
static char metaAlbum[96]  = "";
static char playState[16]  = "none";

// Artwork staging:
//   artBuf holds a decoded RGB565 bitmap. Host streams chunks into artBuf; on
//   ARTEND we commit (artReady=true, artW/artH locked), and the next redraw
//   blits it. Staging is separate from the committed geometry so a partial /
//   aborted transfer can never corrupt what's on screen.
static constexpr int kArtMaxPixels = kArtW * kArtH;
static uint16_t artBuf[kArtMaxPixels];

static bool     artReady       = false;
static int      artCommittedW  = 0;
static int      artCommittedH  = 0;

static bool     artRxActive    = false;
static int      artRxW         = 0;
static int      artRxH         = 0;
static int      artRxChunks    = 0;
static int      artRxBytesTotal= 0;
static int      artRxBytesGot  = 0;
static uint32_t artRxHash      = 0;

volatile int16_t gEncoderSteps = 0;
volatile uint8_t gLastClk      = 0;

static bool     lastButtonDown     = false;
static uint32_t buttonDownAtMs     = 0;
static uint32_t buttonReleasedAtMs = 0;

// Largest host line is one A\t<idx>\t<base64> chunk. We cap chunk binary at
// 256 bytes => ~344 base64 chars. 768B leaves comfortable slack.
static constexpr int kSerialBufSize = 768;
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

// Same sanitizer philosophy as claude3: scrub every printable string down to
// 7-bit ASCII, collapse whitespace runs, and trim edges. Keeps the default
// GFX font from rendering CP437 "CJK" glyphs when the host accidentally
// pushes unicode (em-dashes, smart quotes, etc).
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
    else                        out = ' ';
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

static void scrubForGfxPrint(char* s) {
  if (!s) return;
  for (; *s; ++s) {
    unsigned char u = (unsigned char)*s;
    if (u < 0x20 || u > 0x7e) *s = ' ';
  }
}

static uint16_t stateAccent() {
  if (strcmp(playState, "playing") == 0) return ST77XX_GREEN;
  if (strcmp(playState, "paused")  == 0) return COL_CLAUDE;
  if (strcmp(playState, "stopped") == 0) return COL_GREY;
  return COL_DKGREY;
}

// Base64 decode: standard alphabet, tolerates '=' padding, ignores whitespace.
// Writes into `out` up to `outCap` and returns bytes written, or -1 on error.
static int8_t b64Val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  if (c == '=') return -2;  // padding sentinel
  return -1;
}

static int decodeBase64(const char* src, int srcLen,
                        uint8_t* out, int outCap) {
  int acc = 0;
  int bits = 0;
  int n = 0;
  for (int i = 0; i < srcLen; i++) {
    char c = src[i];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    int8_t v = b64Val(c);
    if (v == -2) break;            // padding -> we're done
    if (v < 0)  return -1;         // garbage
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= outCap) return -1;
      out[n++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return n;
}

// ---- draw ----------------------------------------------------------------

static inline void fillArea(int16_t x, int16_t y, int16_t w, int16_t h,
                            uint16_t c = ST77XX_BLACK) {
  tft.fillRect(x, y, w, h, c);
}

static void drawHeader() {
  fillArea(0, 0, kDispW, kDiv1Y);
  tft.fillCircle(kPad + 4, kHeaderY + 8, 4, stateAccent());

  tft.setTextSize(kBigFontSz);
  tft.setTextColor(COL_WHITE);
  tft.setCursor(kPad + 14, kHeaderY);
  tft.print("Music");

  // State label right-aligned in the header.
  char right[16];
  strncpy(right, playState, sizeof(right) - 1);
  right[sizeof(right) - 1] = '\0';
  scrubForGfxPrint(right);
  tft.setTextSize(kBigFontSz);
  tft.setTextColor(COL_GREY);
  int w = (int)strlen(right) * kBigChrPx;
  int x = kDispW - kPad - w;
  if (x < kPad) x = kPad;
  tft.setCursor(x, kHeaderY + 4);
  tft.print(right);

  tft.drawFastHLine(kPad, kDiv1Y, kInnerW, COL_DIVIDER);
}

// Wrap `s` across at most `maxRows` rows of `cols` glyphs. Soft-wrap at
// spaces, hard-wrap past the limit. Draws each row with the configured text
// size/color. `maxRows == 1` falls back to plain clipped print + "...".
static void drawWrapped(const char* s, int16_t x, int16_t y,
                        int cols, int lineH, int maxRows,
                        uint8_t fontSize, uint16_t color) {
  if (!s || s[0] == '\0' || cols <= 0 || maxRows <= 0) return;
  tft.setTextSize(fontSize);
  tft.setTextColor(color);

  char tmp[160];
  strncpy(tmp, s, sizeof(tmp) - 1);
  tmp[sizeof(tmp) - 1] = '\0';
  scrubForGfxPrint(tmp);

  const int len = (int)strlen(tmp);
  if (len == 0) return;

  int i = 0;
  for (int row = 0; row < maxRows && i < len; row++) {
    int scanEnd = i + cols;
    if (scanEnd > len) scanEnd = len;

    int lineEnd;
    if (scanEnd >= len) {
      lineEnd = len;
    } else {
      int brk = -1;
      for (int j = scanEnd; j > i; j--) {
        if (tmp[j] == ' ') { brk = j; break; }
      }
      lineEnd = (brk > i) ? brk : scanEnd;
    }

    int n = lineEnd - i;
    if (n > cols) n = cols;

    // Last row and more text left => leave "..." to signal truncation.
    const bool hasMore = (lineEnd < len);
    if (row == maxRows - 1 && hasMore && n >= 3) {
      char line[64];
      int take = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
      memcpy(line, tmp + i, take);
      line[take] = '\0';
      // replace last 3 chars with "..."
      int L = (int)strlen(line);
      if (L >= 3) {
        line[L - 1] = '.';
        line[L - 2] = '.';
        line[L - 3] = '.';
      }
      tft.setCursor(x, y + row * lineH);
      tft.print(line);
      return;
    }

    char line[64];
    int take = n < (int)sizeof(line) - 1 ? n : (int)sizeof(line) - 1;
    memcpy(line, tmp + i, take);
    line[take] = '\0';
    tft.setCursor(x, y + row * lineH);
    tft.print(line);

    i = lineEnd;
    while (i < len && tmp[i] == ' ') i++;
  }
}

static void drawArt() {
  // Art frame: thin divider around the region so the slot is visible even
  // when no artwork has been pushed yet.
  tft.drawRect(kArtX - 1, kArtY - 1, kArtW + 2, kArtH + 2, COL_DIVIDER);
  if (artReady && artCommittedW > 0 && artCommittedH > 0) {
    // Center any sub-100x100 image inside the slot.
    int16_t offX = kArtX + (kArtW - artCommittedW) / 2;
    int16_t offY = kArtY + (kArtH - artCommittedH) / 2;
    // Clear around the image in case the previous art was larger.
    if (artCommittedW < kArtW || artCommittedH < kArtH) {
      fillArea(kArtX, kArtY, kArtW, kArtH);
    }
    tft.drawRGBBitmap(offX, offY, artBuf, artCommittedW, artCommittedH);
  } else {
    fillArea(kArtX, kArtY, kArtW, kArtH);
    // Placeholder: small note glyph approximation with a circle + stem.
    const int cx = kArtX + kArtW / 2;
    const int cy = kArtY + kArtH / 2;
    tft.fillCircle(cx - 8, cy + 8, 6, COL_DKGREY);
    tft.drawFastVLine(cx - 2, cy - 18, 26, COL_DKGREY);
    tft.fillRect(cx - 2, cy - 20, 14, 4, COL_DKGREY);
    tft.setTextSize(kSmallFontSz);
    tft.setTextColor(COL_DKGREY);
    tft.setCursor(kArtX + 12, kArtY + kArtH - 12);
    tft.print("no artwork");
  }
}

static void drawMeta() {
  // Single wipe covering the full meta column from header divider to footer
  // divider so stale longer values never bleed through between frames.
  fillArea(kMetaX, kBodyY, kMetaW, kDiv2Y - kBodyY);

  // Artist: label (size 1, dim) + up to 2 rows of value (size 2).
  //   label at kArtY+2  = 48
  //   value at kArtY+14 = 60  (2 rows * 18 = 36  -> ends 96)
  tft.setTextSize(kSmallFontSz);
  tft.setTextColor(COL_DKGREY);
  tft.setCursor(kMetaX, kArtY + 2);
  tft.print("ARTIST");

  const char* artist = metaArtist[0] ? metaArtist : "-";
  drawWrapped(artist, kMetaX, kArtY + 14,
              kMetaBigCols, kBigLineH, 2,
              kBigFontSz, COL_WHITE);

  // Title: label + up to 2 rows of value (size 2, accent color).
  //   label at kArtY+54 = 100
  //   value at kArtY+66 = 112 (2 rows * 18 = 36 -> ends 148)
  tft.setTextSize(kSmallFontSz);
  tft.setTextColor(COL_DKGREY);
  tft.setCursor(kMetaX, kArtY + 54);
  tft.print("TITLE");

  const char* title = metaTitle[0] ? metaTitle : "-";
  drawWrapped(title, kMetaX, kArtY + 66,
              kMetaBigCols, kBigLineH, 2,
              kBigFontSz, COL_ACCENT);

  // Album: label + up to 2 rows of value (size 1, grey).
  //   label at kArtY+106 = 152
  //   value at kArtY+118 = 164 (2 rows * 10 = 20 -> ends 184; art ends 186)
  tft.setTextSize(kSmallFontSz);
  tft.setTextColor(COL_DKGREY);
  tft.setCursor(kMetaX, kArtY + 106);
  tft.print("ALBUM");

  const char* album = metaAlbum[0] ? metaAlbum : "-";
  drawWrapped(album, kMetaX, kArtY + 118,
              kMetaSmallCols, kSmallLineH, 2,
              kSmallFontSz, COL_GREY);
}

static void drawFooter() {
  fillArea(0, kDiv2Y, kDispW, kDispH - kDiv2Y);
  tft.drawFastHLine(kPad, kDiv2Y, kInnerW, COL_DIVIDER);

  // Left: a bracketed primary action mirroring claude3's footer style.
  tft.setTextSize(kBigFontSz);
  tft.setTextColor(stateAccent());
  tft.setCursor(kPad, kFootY + 2);
  if (strcmp(playState, "playing") == 0) tft.print("[ pause ]");
  else                                    tft.print("[ play  ]");

  // Right: compact hint.
  const char* hint = "turn: skip  press: play/pause";
  tft.setTextSize(kSmallFontSz);
  tft.setTextColor(COL_GREY);
  int w = (int)strlen(hint) * kSmallChrPx;
  int x = kDispW - kPad - w;
  int minX = kPad + 9 * kBigChrPx + 4;  // past the [ play ] label
  if (x < minX) x = minX;
  tft.setCursor(x, kFootY + 6);
  tft.print(hint);
}

static void redrawAll() {
  tft.fillScreen(ST77XX_BLACK);
  drawHeader();
  drawArt();
  drawMeta();
  drawFooter();
}

// ---- protocol ------------------------------------------------------------

// Parse "META\t<artist>\t<title>\t<album>" (tabs already stripped as separators).
static void parseMetaLine(char* args) {
  char* save = nullptr;
  char* a = strtok_r(args, "\t", &save);
  char* t = strtok_r(nullptr, "\t", &save);
  char* al = strtok_r(nullptr, "\t", &save);

  strncpy(metaArtist, a ? a : "", sizeof(metaArtist) - 1);
  metaArtist[sizeof(metaArtist) - 1] = '\0';
  strncpy(metaTitle, t ? t : "", sizeof(metaTitle) - 1);
  metaTitle[sizeof(metaTitle) - 1] = '\0';
  strncpy(metaAlbum, al ? al : "", sizeof(metaAlbum) - 1);
  metaAlbum[sizeof(metaAlbum) - 1] = '\0';

  sanitizeAscii(metaArtist);
  sanitizeAscii(metaTitle);
  sanitizeAscii(metaAlbum);

  drawMeta();
}

static void parseStateLine(const char* args) {
  strncpy(playState, args ? args : "none", sizeof(playState) - 1);
  playState[sizeof(playState) - 1] = '\0';
  sanitizeAscii(playState);
  if (playState[0] == '\0') strcpy(playState, "none");
  drawHeader();
  drawFooter();
}

static uint32_t parseHash(const char* s) {
  if (!s) return 0;
  return (uint32_t)strtoul(s, nullptr, 10);
}

static void beginArtRx(char* args) {
  // ART\t<w>\t<h>\t<nchunks>\t<hash>
  char* save = nullptr;
  char* w  = strtok_r(args, "\t", &save);
  char* h  = strtok_r(nullptr, "\t", &save);
  char* nc = strtok_r(nullptr, "\t", &save);
  char* hs = strtok_r(nullptr, "\t", &save);

  int W = w ? atoi(w) : 0;
  int H = h ? atoi(h) : 0;
  int N = nc ? atoi(nc) : 0;
  uint32_t hash = parseHash(hs);

  if (W <= 0 || H <= 0 || W > kArtW || H > kArtH || N <= 0) {
    artRxActive = false;
    return;
  }
  artRxActive    = true;
  artRxW         = W;
  artRxH         = H;
  artRxChunks    = N;
  artRxBytesTotal= W * H * 2;
  artRxBytesGot  = 0;
  artRxHash      = hash;
}

static void handleArtChunk(char* args) {
  if (!artRxActive) return;
  // A\t<idx>\t<base64>
  char* save = nullptr;
  char* idxS = strtok_r(args, "\t", &save);
  char* b64  = strtok_r(nullptr, "\t", &save);
  if (!idxS || !b64) { artRxActive = false; return; }

  const int idx = atoi(idxS);
  if (idx < 0 || idx >= artRxChunks) { artRxActive = false; return; }

  const int b64Len = (int)strlen(b64);
  const int cap    = artRxBytesTotal - artRxBytesGot;
  if (cap <= 0) { artRxActive = false; return; }

  uint8_t* dst = reinterpret_cast<uint8_t*>(artBuf) + artRxBytesGot;
  const int n = decodeBase64(b64, b64Len, dst, cap);
  if (n <= 0) { artRxActive = false; return; }
  artRxBytesGot += n;
}

static void finalizeArt(char* args) {
  if (!artRxActive) return;
  const uint32_t hash = parseHash(args);
  if (hash != artRxHash || artRxBytesGot != artRxBytesTotal) {
    artRxActive = false;
    return;
  }
  // RGB565 on the wire is big-endian per pixel (high byte first). Flip to the
  // little-endian layout drawRGBBitmap expects on this controller.
  uint8_t* raw = reinterpret_cast<uint8_t*>(artBuf);
  const int pixels = artRxW * artRxH;
  for (int i = 0; i < pixels; i++) {
    uint8_t hi = raw[i * 2 + 0];
    uint8_t lo = raw[i * 2 + 1];
    artBuf[i] = ((uint16_t)hi << 8) | lo;
  }
  artReady      = true;
  artCommittedW = artRxW;
  artCommittedH = artRxH;
  artRxActive   = false;
  drawArt();
}

static void clearArt() {
  artRxActive   = false;
  artReady      = false;
  artCommittedW = 0;
  artCommittedH = 0;
  drawArt();
}

static void handleLine(char* line) {
  if (line[0] == '\0') return;
  char* tab = strchr(line, '\t');
  char* args = nullptr;
  if (tab) { *tab = '\0'; args = tab + 1; }

  if (strcmp(line, "META") == 0 && args) {
    parseMetaLine(args);
  } else if (strcmp(line, "STATE") == 0) {
    parseStateLine(args ? args : "");
  } else if (strcmp(line, "ART") == 0 && args) {
    beginArtRx(args);
  } else if (strcmp(line, "A") == 0 && args) {
    handleArtChunk(args);
  } else if (strcmp(line, "ARTEND") == 0) {
    finalizeArt(args ? args : "");
  } else if (strcmp(line, "ARTCLR") == 0) {
    clearArt();
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
      // Overflow: drop the in-progress line to avoid rendering a frame from
      // truncated bytes. Any active art RX is invalidated too.
      serialLen = 0;
      artRxActive = false;
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

  // One serial command per detent, preserving fast multi-click rotations.
  // Positive = CW = NEXT, Negative = CCW = PREV.
  if (steps > 0) {
    for (int i = 0; i < steps; i++) Serial.println("NEXT");
  } else {
    for (int i = 0; i < -steps; i++) Serial.println("PREV");
  }
}

static void pollButton() {
  const bool down = (digitalRead(SW_PIN) == LOW);
  const uint32_t now = millis();
  if (down && !lastButtonDown) {
    if (now - buttonReleasedAtMs >= kButtonDebounceMs) {
      buttonDownAtMs = now;
      lastButtonDown = true;
    }
  } else if (!down && lastButtonDown) {
    if (now - buttonDownAtMs >= kButtonDebounceMs) {
      const uint32_t held = now - buttonDownAtMs;
      buttonReleasedAtMs = now;
      lastButtonDown = false;
      if (held < kButtonLongMs) {
        Serial.println("PLAY_PAUSE");
      }
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

  strncpy(metaArtist, "",              sizeof(metaArtist) - 1);
  strncpy(metaTitle,  "Connect Mac bridge", sizeof(metaTitle) - 1);
  strncpy(metaAlbum,  "",              sizeof(metaAlbum) - 1);

  redrawAll();

  Serial.println("HELLO\tminideck-music");
}

void loop() {
  pollSerial();
  drainEncoder();
  pollButton();
  delay(2);
}
