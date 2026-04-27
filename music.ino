#include <Adafruit_GFX.h>
#include <SPI.h>
#include <Adafruit_ST7735.h>
#include <string.h>

// ST7735 128x128 (init string says 144 green tab — common cheap board id)
#define TFT_CS 5
#define TFT_RST 4
#define TFT_A0 2
#define TFT_LED 15
// SPI: MOSI=23, SCK=18 (VSPI default on many ESP32 boards)

// KY-040 rotary encoder
#define CLK_PIN 14
#define DT_PIN 12
#define SW_PIN 13

static constexpr uint32_t kButtonDebounceMs = 45;
static constexpr uint32_t kButtonRepeatMs = 280;
static constexpr uint32_t kStatusClearMs = 750;

// Layout (128x128 ST7735, rotation 1)
static constexpr int16_t kMetaY = 6;
static constexpr int16_t kMenuY = 50;
static constexpr int16_t kStatusY = 100;

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_A0, TFT_RST);

static const char* const kOptions[] = {
    "Vol UP", "Vol DOWN", "Next", "Prev", "Play",
};
static constexpr int kOptionCount = sizeof(kOptions) / sizeof(kOptions[0]);

static int menuIndex = 0;
static int lastMenuIndex = -1;

// Encoder: accumulate steps in ISR, consume in loop() to keep ISR tiny and avoid
// tearing when wrapping menuIndex.
volatile int16_t gEncoderSteps = 0;
volatile uint8_t gLastClk = 0;

static unsigned long lastButtonActionMs = 0;
static unsigned long lastButtonReleaseMs = 0;
static bool lastButtonDown = false;

// Now playing from host (serial_music_bridge.py): line "M\tartist\ttitle\n"
static char metaArtist[80];
static char metaTitle[80];

static char serialLineBuf[180];
static uint8_t serialLineLen = 0;

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

static int wrapIndex(int idx, int n) { return (idx % n + n) % n; }

static void drainEncoderIntoMenu() {
  int16_t steps = 0;
  noInterrupts();
  steps = gEncoderSteps;
  gEncoderSteps = 0;
  interrupts();

  if (steps == 0) return;
  menuIndex = wrapIndex(menuIndex + (int)steps, kOptionCount);
}

static void printTwoLines(const char* s, int16_t x, int16_t y0, uint16_t color) {
  tft.setTextSize(1);
  tft.setTextColor(color);
  char line[22];
  size_t n = strlen(s);
  if (n > 40) n = 40;
  const size_t n1 = n > 20 ? 20 : n;
  memcpy(line, s, n1);
  line[n1] = '\0';
  tft.setCursor(x, y0);
  tft.print(line);
  if (n > 20) {
    size_t rest = n - 20;
    if (rest > 20) rest = 20;
    memcpy(line, s + 20, rest);
    line[rest] = '\0';
    tft.setCursor(x, y0 + 8);
    tft.print(line);
  }
}

static void drawNowPlayingBlock() {
  tft.fillRect(6, kMetaY, 116, 38, ST77XX_BLACK);
  printTwoLines(metaArtist, 8, kMetaY, ST77XX_WHITE);
  printTwoLines(metaTitle, 8, kMetaY + 18, ST77XX_YELLOW);
}

static void drawMenuSelection() {
  tft.fillRect(6, kMenuY, 116, 36, ST77XX_BLACK);
  tft.setCursor(8, kMenuY + 4);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_CYAN);
  tft.print(kOptions[menuIndex]);
  lastMenuIndex = menuIndex;
}

static void flashStatus(const char* text) {
  tft.fillRect(6, kStatusY, 116, 20, ST77XX_BLACK);
  tft.setCursor(8, kStatusY);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.print(text);
}

static void sendSerialCommand(int idx) {
  const char* line = nullptr;
  switch (idx) {
    case 0:
      line = "VOL_UP";
      break;
    case 1:
      line = "VOL_DOWN";
      break;
    case 2:
      line = "NEXT";
      break;
    case 3:
      line = "PREV";
      break;
    case 4:
      line = "PLAY_PAUSE";
      break;
    default:
      return;
  }
  Serial.println(line);
  Serial.flush();
}

static void applyHostMetaLine(char* line) {
  // line = "artist\ttitle" (tab-separated, no leading M\t — caller strips prefix)
  char* tab = strchr(line, '\t');
  if (!tab) return;
  *tab = '\0';
  strncpy(metaArtist, line, sizeof(metaArtist) - 1);
  metaArtist[sizeof(metaArtist) - 1] = '\0';
  strncpy(metaTitle, tab + 1, sizeof(metaTitle) - 1);
  metaTitle[sizeof(metaTitle) - 1] = '\0';
  drawNowPlayingBlock();
}

static void pollHostSerial() {
  while (Serial.available() > 0) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      serialLineBuf[serialLineLen] = '\0';
      serialLineLen = 0;
      if (serialLineBuf[0] == 'M' && serialLineBuf[1] == '\t') {
        applyHostMetaLine(serialLineBuf + 2);
      }
      continue;
    }
    if (serialLineLen < sizeof(serialLineBuf) - 1) {
      serialLineBuf[serialLineLen++] = c;
    } else {
      serialLineLen = 0;
    }
  }
}

void setup() {
  Serial.begin(115200);

  // Bring up the TFT first so any later failure still shows *something*.
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  tft.initR(INITR_144GREENTAB);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(5, 5, 120, 120, ST77XX_BLUE);

  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(1);
  tft.setCursor(10, 10);
  tft.println("Media / serial");
  tft.setCursor(10, 24);
  tft.println("KY-040 + ST7735");
  delay(500);

  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(5, 5, 120, 120, ST77XX_BLUE);

  strncpy(metaArtist, "--", sizeof(metaArtist) - 1);
  metaArtist[sizeof(metaArtist) - 1] = '\0';
  strncpy(metaTitle, "Connect Mac bridge", sizeof(metaTitle) - 1);
  metaTitle[sizeof(metaTitle) - 1] = '\0';
  drawNowPlayingBlock();

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);
  pinMode(SW_PIN, INPUT_PULLUP);

  // If SW is held / shorted at boot, swallow that press non-blockingly.
  lastButtonDown = (digitalRead(SW_PIN) == LOW);
  lastButtonReleaseMs = millis();
  lastButtonActionMs = millis();

  gLastClk = (uint8_t)(digitalRead(CLK_PIN) != 0);
  attachInterrupt(digitalPinToInterrupt(CLK_PIN), onEncoderClk, CHANGE);

  // Force an initial menu render (menuIndex starts at 0, lastMenuIndex at -1).
  drawMenuSelection();
}

void loop() {
  pollHostSerial();
  drainEncoderIntoMenu();

  if (menuIndex != lastMenuIndex) {
    drawMenuSelection();
  }

  const bool down = (digitalRead(SW_PIN) == LOW);
  const unsigned long now = millis();

  if (down) {
    if (!lastButtonDown) {
      // Fresh press: debounce from last release, not last fire (so quick taps work).
      if (now - lastButtonReleaseMs >= kButtonDebounceMs) {
        sendSerialCommand(menuIndex);
        flashStatus("Serial sent");
        lastButtonActionMs = now;
      }
    } else if (now - lastButtonActionMs >= kButtonRepeatMs) {
      sendSerialCommand(menuIndex);
      flashStatus("Serial sent");
      lastButtonActionMs = now;
    }
  } else {
    if (lastButtonDown) {
      lastButtonReleaseMs = now;
    }
    if ((now - lastButtonActionMs) >= kStatusClearMs) {
      tft.fillRect(6, kStatusY, 116, 20, ST77XX_BLACK);
    }
  }
  lastButtonDown = down;

  delay(2);
}
