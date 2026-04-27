#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>

const int PIN_LCD_CS = SS;
const int PIN_LCD_DC = 4;
const int PIN_LCD_RST = 2;
const int PIN_LCD_BL = 15;

Adafruit_ST7789 tft = Adafruit_ST7789(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);

int counter = 0;
int lastStateCLK;

void setup()
{
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);
  pinMode(32, INPUT_PULLUP);
  pinMode(33, INPUT_PULLUP);
  lastStateCLK = digitalRead(32);

  tft.init(240, 320);
  tft.setRotation(1);
  tft.fillScreen(ST77XX_BLACK);
}

void loop()
{
  int currentStateCLK = digitalRead(32);
  if (currentStateCLK != lastStateCLK && currentStateCLK == LOW) {
    if (digitalRead(33) != currentStateCLK) {
      counter++;
    } else {
      counter--;
    }
    tft.fillScreen(ST77XX_BLACK);
    tft.setCursor(60, 110);
    tft.setTextColor(ST77XX_WHITE);
    tft.setTextSize(3);
    tft.print("Value: ");
    tft.println(counter);
  }
  lastStateCLK = currentStateCLK;
  delay(1);
}