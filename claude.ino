// minideck — generic context-aware menu / PTT terminal.
//
// The firmware is intentionally dumb: the host pushes header text, menu
// items and a "state" name; the device renders them, reports button +
// encoder events, and lets the host decide what they mean.
//
// Wire protocol (newline-delimited, fields separated by \t):
//
//   host -> device:
//     CTX\t<line1>\t<line2>          two-line header at the top
//     MENU\t<label1>\t<label2>...    replaces menu options
//     SEL\t<index>                   force selected index
//     STATE\t<name>                  idle | working | awaiting | recording
//     STATUS\t<text>                 transient flash message
//     CLR                            full repaint
//
//   device -> host:
//     HELLO\tminideck-1              once after boot
//     DOWN                           button pressed (debounced)
//     UP\t<ms_held>\t<menu_index>    button released
//     TURN\t<delta>                  encoder rotated, signed step count

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <stdlib.h>
#include <string.h>

// Pins — same wiring as music.ino so no rewire needed.
#define TFT_CS 5
#define TFT_RST 4
#define TFT_A0 2
#define TFT_LED 15

#define CLK_PIN 14
#define DT_PIN 12
#define SW_PIN 13

static constexpr uint32_t kButtonDebounceMs = 35;
// Status line is persistent: it stays until the host replaces it or
// explicitly clears it with an empty STATUS\t payload. The host owns
// its lifetime so it can pin things like the last transcript.

// Layout (128x128, rotation 1)
static constexpr int16_t kHeader1Y = 6;
static constexpr int16_t kHeader2Y = 18;
static constexpr int16_t kMenuY = 48;
static constexpr int16_t kCountY = 78;
static constexpr int16_t kStatusY = 96;
static constexpr int16_t kStatus2Y = 106;  // second status row for wrapped text
static constexpr int16_t kHintY = 116;
static constexpr int kStatusMaxChars = 20;  // ~120px / 6px per glyph

// 16-bit RGB565 colors not in ST77XX_*.
static constexpr uint16_t COL_GREY = 0x8410;    // ~50%
static constexpr uint16_t COL_DKGREY = 0x4208;  // ~25%

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_A0, TFT_RST);

// Menu storage: all labels concatenated as null-terminated strings in one
// buffer; menuLabels[] indexes into it. Single allocation, no fragmentation.
static constexpr int kMaxMenuItems = 12;
static constexpr int kMenuBufSize = 384;
static char menuBuf[kMenuBufSize];
static const char* menuLabels[kMaxMenuItems];
static int menuCount = 0;
static int menuIndex = 0;
static int lastDrawnIndex = -1;
static int lastDrawnCount = -1;

// Header + state
static char headerLine1[24] = "minideck";
static char headerLine2[24] = "waiting for host";
static char stateName[16] = "idle";

// Status line
static char statusText[64] = "";
static bool statusActive = false;

// Encoder ISR state
volatile int16_t gEncoderSteps = 0;
volatile uint8_t gLastClk = 0;

// Button edge tracking
static bool lastButtonDown = false;
static uint32_t buttonDownAtMs = 0;
static uint32_t buttonReleasedAtMs = 0;

// Inbound serial line buffer
static constexpr int kSerialBufSize = 512;
static char serialBuf[kSerialBufSize];
static int serialLen = 0;

void IRAM_ATTR onEncoderClk() {
  const int clk = digitalRead(CLK_PIN);
  if (clk != (int)gLastClk && clk == LOW) {
    if (digitalRead(DT_PIN) != clk) {
      gEncoderSteps++;
    } else {
      gEncoderSteps--;
    }
  }
  gLastClk = (uint8_t)(clk != 0);
}

static int wrapIndex(int v, int n) {
  if (n <= 0) return 0;
  return (v % n + n) % n;
}

// Color/hint pulled from current state name. Keep this the only place that
// branches on state — everything else just calls these helpers.
static uint16_t menuColor() {
  if (strcmp(stateName, "working") == 0) return ST77XX_ORANGE;
  if (strcmp(stateName, "awaiting") == 0) return ST77XX_GREEN;
  if (strcmp(stateName, "recording") == 0) return ST77XX_RED;
  return ST77XX_CYAN;  // idle / unknown
}

static uint16_t borderColor() {
  if (strcmp(stateName, "awaiting") == 0) return ST77XX_GREEN;
  if (strcmp(stateName, "recording") == 0) return ST77XX_RED;
  if (strcmp(stateName, "working") == 0) return ST77XX_ORANGE;
  return ST77XX_BLUE;
}

static const char* hintForState() {
  if (strcmp(stateName, "awaiting") == 0) return "tap = approve";
  if (strcmp(stateName, "recording") == 0) return "release to send";
  return "hold to talk";  // idle, working, or unknown
}

static void clearArea(int16_t x, int16_t y, int16_t w, int16_t h) {
  tft.fillRect(x, y, w, h, ST77XX_BLACK);
}

static void drawBorder() {
  tft.drawRect(2, 2, 124, 124, borderColor());
}

static void drawHeader() {
  clearArea(4, kHeader1Y - 1, 120, 24);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.setCursor(6, kHeader1Y);
  tft.print(headerLine1);
  tft.setTextColor(COL_GREY);
  tft.setCursor(6, kHeader2Y);
  tft.print(headerLine2);
}

static void drawMenu() {
  clearArea(4, kMenuY - 4, 120, 38);
  if (menuCount == 0) {
    tft.setTextSize(1);
    tft.setTextColor(COL_GREY);
    tft.setCursor(6, kMenuY);
    tft.print("(no menu)");
    lastDrawnIndex = -1;
    lastDrawnCount = 0;
    return;
  }
  // Big text (size 2 = 12x16). Truncate to 10 chars to leave a margin.
  tft.setTextSize(2);
  tft.setTextColor(menuColor());
  const char* label = menuLabels[menuIndex];
  char buf[12];
  size_t n = strlen(label);
  if (n > 10) n = 10;
  memcpy(buf, label, n);
  buf[n] = '\0';
  tft.setCursor(6, kMenuY);
  tft.print(buf);

  // "n/N" chip in the corner
  if (menuCount > 1) {
    char chip[12];
    snprintf(chip, sizeof(chip), "%d/%d", menuIndex + 1, menuCount);
    tft.setTextSize(1);
    tft.setTextColor(COL_DKGREY);
    const int chipW = (int)strlen(chip) * 6;
    tft.setCursor(120 - chipW, kCountY);
    tft.print(chip);
  }
  lastDrawnIndex = menuIndex;
  lastDrawnCount = menuCount;
}

static void drawHint() {
  clearArea(4, kHintY, 120, 8);
  tft.setTextSize(1);
  tft.setTextColor(COL_GREY);
  tft.setCursor(6, kHintY);
  tft.print(hintForState());
}

static void drawStatus() {
  clearArea(4, kStatusY, 120, 18);
  if (!statusActive) return;
  tft.setTextSize(1);
  tft.setTextColor(menuColor());

  const int len = (int)strlen(statusText);
  if (len <= kStatusMaxChars) {
    tft.setCursor(6, kStatusY);
    tft.print(statusText);
    return;
  }

  // Prefer to break at the last space within line 1; fall back to a
  // hard wrap if the string is one long token (e.g. a file path).
  int breakAt = kStatusMaxChars;
  for (int i = kStatusMaxChars; i > 8; i--) {
    if (statusText[i] == ' ') {
      breakAt = i;
      break;
    }
  }

  char buf[kStatusMaxChars + 1];
  memcpy(buf, statusText, breakAt);
  buf[breakAt] = '\0';
  tft.setCursor(6, kStatusY);
  tft.print(buf);

  int start2 = breakAt;
  while (statusText[start2] == ' ') start2++;
  int rem = len - start2;
  int copy = rem > kStatusMaxChars ? kStatusMaxChars : rem;
  memcpy(buf, statusText + start2, copy);
  buf[copy] = '\0';
  // Still overflowing? Trail an ellipsis so the user knows.
  if (rem > kStatusMaxChars && copy >= 3) {
    buf[copy - 3] = '.';
    buf[copy - 2] = '.';
    buf[copy - 1] = '.';
  }
  tft.setCursor(6, kStatus2Y);
  tft.print(buf);
}

static void redrawAll() {
  tft.fillScreen(ST77XX_BLACK);
  drawBorder();
  drawHeader();
  drawMenu();
  drawStatus();
  drawHint();
}

static void setStatus(const char* text) {
  if (text == nullptr || text[0] == '\0') {
    if (!statusActive) return;
    statusActive = false;
    statusText[0] = '\0';
    drawStatus();
    return;
  }
  // Skip redraw (and its flicker) if the host re-pushes the same text.
  if (statusActive && strcmp(statusText, text) == 0) return;
  strncpy(statusText, text, sizeof(statusText) - 1);
  statusText[sizeof(statusText) - 1] = '\0';
  statusActive = true;
  drawStatus();
}

static void parseMenuLine(char* args) {
  menuCount = 0;
  size_t bufUsed = 0;
  char* save = nullptr;
  char* tok = strtok_r(args, "\t", &save);
  while (tok && menuCount < kMaxMenuItems) {
    size_t l = strlen(tok);
    if (bufUsed + l + 1 > kMenuBufSize) break;
    memcpy(menuBuf + bufUsed, tok, l + 1);
    menuLabels[menuCount++] = menuBuf + bufUsed;
    bufUsed += l + 1;
    tok = strtok_r(nullptr, "\t", &save);
  }
  if (menuIndex >= menuCount) menuIndex = 0;
  drawMenu();
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
  drawHeader();
}

static void handleLine(char* line) {
  if (line[0] == '\0') return;
  char* tab = strchr(line, '\t');
  char* args = nullptr;
  if (tab) {
    *tab = '\0';
    args = tab + 1;
  }
  if (strcmp(line, "CTX") == 0 && args) {
    parseCtxLine(args);
  } else if (strcmp(line, "MENU") == 0 && args) {
    parseMenuLine(args);
  } else if (strcmp(line, "SEL") == 0 && args) {
    int v = atoi(args);
    if (menuCount > 0) {
      menuIndex = wrapIndex(v, menuCount);
      drawMenu();
    }
  } else if (strcmp(line, "STATE") == 0 && args) {
    strncpy(stateName, args, sizeof(stateName) - 1);
    stateName[sizeof(stateName) - 1] = '\0';
    // State drives colors *and* the hint, so repaint everything that
    // depends on it.
    drawBorder();
    drawMenu();
    drawHint();
  } else if (strcmp(line, "STATUS") == 0 && args) {
    setStatus(args);
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
      serialLen = 0;  // overflow — drop the line
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
  if (menuCount > 0) {
    menuIndex = wrapIndex(menuIndex + (int)steps, menuCount);
  }
  // Always forward raw turns so the host can use them for things like
  // scrollback navigation or "cancel recording on twist".
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

void setup() {
  Serial.begin(115200);

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  tft.initR(INITR_144GREENTAB);
  tft.setRotation(1);
  // Text that exceeds the display width should truncate, not wrap into
  // the row below (which would trample other UI rows).
  tft.setTextWrap(false);
  tft.fillScreen(ST77XX_BLACK);

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);
  pinMode(SW_PIN, INPUT_PULLUP);

  gLastClk = (uint8_t)(digitalRead(CLK_PIN) != 0);
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), onEncoderClk, CHANGE);

  // If button is shorted at boot, swallow that as a "release".
  lastButtonDown = (digitalRead(SW_PIN) == LOW);
  buttonReleasedAtMs = millis();
  buttonDownAtMs = millis();

  redrawAll();

  // Identify ourselves so the host can do its first push.
  Serial.println("HELLO\tminideck-1");
}

void loop() {
  pollSerial();
  drainEncoder();
  pollButton();

  if (menuIndex != lastDrawnIndex || menuCount != lastDrawnCount) {
    drawMenu();
  }

  delay(2);
}
