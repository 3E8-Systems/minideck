            // minideck — claude2: ST7789 240x320 (landscape 320x240) variant of claude.ino.
// LCD + encoder CLK/DT match waveshare.ino (demo uses 32/33 only). KY-040 SW on
// GPIO 34 (input-only: no internal pull-up — use module or external pull-up to 3V3).
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
            //
            // Display: Adafruit_ST7789 — same as waveshare.ino (CS=SS, DC=4, RST=2, BL=15).
            // Encoder: CLK=32, DT=33 (waveshare.ino). SW=34 (KY-040, active LOW).

            #include <Adafruit_GFX.h>
            #include <Adafruit_ST7789.h>
            #include <SPI.h>
            #include <stdlib.h>
            #include <string.h>

            const int PIN_LCD_CS = SS;
            const int PIN_LCD_DC = 4;
            const int PIN_LCD_RST = 2;
            const int PIN_LCD_BL = 15;

            #define CLK_PIN 32
            #define DT_PIN 33
            #define SW_PIN 34

            static constexpr uint32_t kButtonDebounceMs = 35;

            // 320x240 after setRotation(1) on 240x320 panel
            static constexpr int16_t kDispW = 320;
            static constexpr int16_t kDispH = 240;
            static constexpr int16_t kPad = 6;
            static constexpr int16_t kInnerW = kDispW - 2 * kPad;

            static constexpr int16_t kHeader1Y = 8;
            static constexpr int16_t kHeader2Y = 26;
            static constexpr int16_t kMenuY = 54;
            static constexpr int16_t kCountY = 96;
            static constexpr int16_t kStatusY = 118;
            static constexpr int16_t kStatus2Y = 136;
            static constexpr int16_t kHintY = 218;
            static constexpr int kStatusMaxChars = 50;  // ~6px/glyph at text size 1

            static constexpr uint16_t COL_GREY = 0x8410;
            static constexpr uint16_t COL_DKGREY = 0x4208;

            Adafruit_ST7789 tft =
            Adafruit_ST7789(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

            static constexpr int kMaxMenuItems = 12;
            static constexpr int kMenuBufSize = 384;
            static char menuBuf[kMenuBufSize];
            static const char* menuLabels[kMaxMenuItems];
            static int menuCount = 0;
            static int menuIndex = 0;
            static int lastDrawnIndex = -1;
            static int lastDrawnCount = -1;

            static char headerLine1[40] = "minideck";
            static char headerLine2[40] = "waiting for host";
            static char stateName[16] = "idle";

            static char statusText[96] = "";
            static bool statusActive = false;

            volatile int16_t gEncoderSteps = 0;
            volatile uint8_t gLastClk = 0;

            static bool lastButtonDown = false;
            static uint32_t buttonDownAtMs = 0;
            static uint32_t buttonReleasedAtMs = 0;

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

            static uint16_t menuColor() {
            if (strcmp(stateName, "working") == 0) return ST77XX_ORANGE;
            if (strcmp(stateName, "awaiting") == 0) return ST77XX_GREEN;
            if (strcmp(stateName, "recording") == 0) return ST77XX_RED;
            return ST77XX_CYAN;
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
            return "hold to talk";
            }

            static void clearArea(int16_t x, int16_t y, int16_t w, int16_t h) {
            tft.fillRect(x, y, w, h, ST77XX_BLACK);
            }

            static void drawBorder() {
            tft.drawRect(2, 2, kDispW - 4, kDispH - 4, borderColor());
            }

            static void drawHeader() {
            clearArea(kPad, kHeader1Y - 2, kInnerW, 36);
            tft.setTextSize(2);
            tft.setTextColor(ST77XX_WHITE);
            tft.setCursor(kPad, kHeader1Y);
            tft.print(headerLine1);
            tft.setTextSize(1);
            tft.setTextColor(COL_GREY);
            tft.setCursor(kPad, kHeader2Y);
            tft.print(headerLine2);
            }

            static void drawMenu() {
            clearArea(kPad, kMenuY - 6, kInnerW, 48);
            if (menuCount == 0) {
            tft.setTextSize(1);
            tft.setTextColor(COL_GREY);
            tft.setCursor(kPad, kMenuY);
            tft.print("(no menu)");
            lastDrawnIndex = -1;
            lastDrawnCount = 0;
            return;
            }
            tft.setTextSize(3);
            tft.setTextColor(menuColor());
            const char* label = menuLabels[menuIndex];
            char buf[20];
            size_t n = strlen(label);
            if (n > 16) n = 16;
            memcpy(buf, label, n);
            buf[n] = '\0';
            tft.setCursor(kPad, kMenuY);
            tft.print(buf);

            if (menuCount > 1) {
            char chip[16];
            snprintf(chip, sizeof(chip), "%d/%d", menuIndex + 1, menuCount);
            tft.setTextSize(2);
            tft.setTextColor(COL_DKGREY);
            const int chipW = (int)strlen(chip) * 12;
            int cx = kDispW - kPad - chipW;
            if (cx < kPad) cx = kPad;
            tft.setCursor(cx, kCountY);
            tft.print(chip);
            }
            lastDrawnIndex = menuIndex;
            lastDrawnCount = menuCount;
            }

            static void drawHint() {
            clearArea(kPad, kHintY, kInnerW, 20);
            tft.setTextSize(2);
            tft.setTextColor(COL_GREY);
            tft.setCursor(kPad, kHintY);
            tft.print(hintForState());
            }

            static void drawStatus() {
            clearArea(kPad, kStatusY - 2, kInnerW, 36);
            if (!statusActive) return;
            tft.setTextSize(1);
            tft.setTextColor(menuColor());

            const int len = (int)strlen(statusText);
            if (len <= kStatusMaxChars) {
            tft.setCursor(kPad, kStatusY);
            tft.print(statusText);
            return;
            }

            int breakAt = kStatusMaxChars;
            for (int i = kStatusMaxChars; i > 12; i--) {
            if (statusText[i] == ' ') {
            breakAt = i;
            break;
            }
            }

            char buf[kStatusMaxChars + 1];
            memcpy(buf, statusText, breakAt);
            buf[breakAt] = '\0';
            tft.setCursor(kPad, kStatusY);
            tft.print(buf);

            int start2 = breakAt;
            while (statusText[start2] == ' ') start2++;
            int rem = len - start2;
            int copy = rem > kStatusMaxChars ? kStatusMaxChars : rem;
            memcpy(buf, statusText + start2, copy);
            buf[copy] = '\0';
            if (rem > kStatusMaxChars && copy >= 3) {
            buf[copy - 3] = '.';
            buf[copy - 2] = '.';
            buf[copy - 1] = '.';
            }
            tft.setCursor(kPad, kStatus2Y);
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
            if (menuCount > 0) {
            menuIndex = wrapIndex(menuIndex + (int)steps, menuCount);
            }
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

            pinMode(PIN_LCD_BL, OUTPUT);
            digitalWrite(PIN_LCD_BL, HIGH);

            tft.init(240, 320);
            tft.setRotation(1);
            tft.setTextWrap(false);
            tft.fillScreen(ST77XX_BLACK);

  pinMode(CLK_PIN, INPUT_PULLUP);
  pinMode(DT_PIN, INPUT_PULLUP);
#if SW_PIN >= 34 && SW_PIN <= 39
  pinMode(SW_PIN, INPUT);  // ESP32: GPIO34–39 have no internal pull-ups
#else
  pinMode(SW_PIN, INPUT_PULLUP);
#endif

            gLastClk = (uint8_t)(digitalRead(CLK_PIN) != 0);
            attachInterrupt(digitalPinToInterrupt(CLK_PIN), onEncoderClk, CHANGE);

            lastButtonDown = (digitalRead(SW_PIN) == LOW);
            buttonReleasedAtMs = millis();
            buttonDownAtMs = millis();

            redrawAll();

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
