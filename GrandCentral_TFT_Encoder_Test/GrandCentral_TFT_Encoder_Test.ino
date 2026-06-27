/*
  Grand Central M4 - SPI TFT + Arduino encoder module test.

  Wiring from the numbered side header / 2-row header:

    TFT VCC             -> 3V first
    TFT GND             -> GND
    TFT SCK / CLK       -> 52
    TFT MOSI / DIN      -> 51
    TFT CS              -> 9
    TFT DC / A0 / RS    -> 8
    TFT RST / RESET     -> 7
    TFT BL / LED        -> 11, or 3V for always-on
    TFT MISO / SDO      -> leave unconnected for first test

    Encoder CLK / A     -> 2
    Encoder DT / B      -> 3
    Encoder SW          -> 4
    Encoder + / VCC     -> 3V
    Encoder GND         -> GND

  Driver selection:
    This sketch defaults to ST7789. If your display is ILI9341, change
    DISPLAY_DRIVER_ST7789 to 0 and DISPLAY_DRIVER_ILI9341 to 1.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>

#define DISPLAY_DRIVER_ST7789  1
#define DISPLAY_DRIVER_ILI9341 0

#if DISPLAY_DRIVER_ST7789
#include <Adafruit_ST7789.h>
#define TFT_BLACK   ST77XX_BLACK
#define TFT_WHITE   ST77XX_WHITE
#define TFT_RED     ST77XX_RED
#define TFT_GREEN   ST77XX_GREEN
#define TFT_BLUE    ST77XX_BLUE
#define TFT_YELLOW  ST77XX_YELLOW
#define TFT_CYAN    ST77XX_CYAN
#define TFT_MAGENTA ST77XX_MAGENTA
#elif DISPLAY_DRIVER_ILI9341
#include <Adafruit_ILI9341.h>
#define TFT_BLACK   ILI9341_BLACK
#define TFT_WHITE   ILI9341_WHITE
#define TFT_RED     ILI9341_RED
#define TFT_GREEN   ILI9341_GREEN
#define TFT_BLUE    ILI9341_BLUE
#define TFT_YELLOW  ILI9341_YELLOW
#define TFT_CYAN    ILI9341_CYAN
#define TFT_MAGENTA ILI9341_MAGENTA
#else
#error "Select one display driver."
#endif

const uint8_t TFT_CS = 9;
const uint8_t TFT_DC = 8;
const uint8_t TFT_RST = 7;
const uint8_t TFT_BL = 11;

const uint8_t ENC_A = 2;
const uint8_t ENC_B = 3;
const uint8_t ENC_SW = 4;

#if DISPLAY_DRIVER_ST7789
Adafruit_ST7789 tft(&SPI, TFT_CS, TFT_DC, TFT_RST);
#else
Adafruit_ILI9341 tft(&SPI, TFT_DC, TFT_CS, TFT_RST);
#endif

volatile int32_t encoderCount = 0;
volatile uint8_t encoderPrev = 0;

bool buttonLast = true;
uint32_t lastRedrawMs = 0;
uint16_t screenW = 0;
uint16_t screenH = 0;

void encoderIsr()
{
  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);
  uint8_t curr = (a << 1) | b;
  uint8_t transition = (encoderPrev << 2) | curr;

  // Valid quadrature transitions. Direction may be reversed depending on module.
  if (transition == 0b0001 || transition == 0b0111 ||
      transition == 0b1110 || transition == 0b1000)
    encoderCount++;
  else if (transition == 0b0010 || transition == 0b1011 ||
           transition == 0b1101 || transition == 0b0100)
    encoderCount--;

  encoderPrev = curr;
}

void drawColorBars()
{
  uint16_t colors[] = {
    TFT_RED, TFT_GREEN, TFT_BLUE, TFT_YELLOW,
    TFT_CYAN, TFT_MAGENTA, TFT_WHITE
  };

  int barW = max(1, screenW / 7);
  for (int i = 0; i < 7; i++)
    tft.fillRect(i * barW, 0, barW, 22, colors[i]);
}

void drawScreen()
{
  noInterrupts();
  int32_t count = encoderCount;
  interrupts();

  bool buttonPressed = (digitalRead(ENC_SW) == LOW);

  tft.fillScreen(TFT_BLACK);

  drawColorBars();

  tft.setTextWrap(false);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(8, 36);
  tft.print("Grand Central M4");

  tft.setTextSize(2);
  tft.setCursor(8, 70);
  tft.print("SPI TFT test");

  tft.setTextSize(2);
  tft.setCursor(8, 112);
  tft.print("Encoder: ");
  tft.print(count);

  tft.setCursor(8, 146);
  tft.print("Button: ");
  tft.print(buttonPressed ? "DOWN" : "UP");

  tft.setTextSize(1);
  tft.setCursor(8, screenH > 18 ? screenH - 18 : 190);
  tft.print("SPI: 52=SCK  51=MOSI  CS=9  DC=8  RST=7");
}

void setup()
{
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, HIGH);

  pinMode(ENC_A, INPUT_PULLUP);
  pinMode(ENC_B, INPUT_PULLUP);
  pinMode(ENC_SW, INPUT_PULLUP);
  encoderPrev = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
  attachInterrupt(digitalPinToInterrupt(ENC_A), encoderIsr, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), encoderIsr, CHANGE);

  Serial.begin(115200);
  delay(500);
  Serial.println("Grand Central M4 TFT + encoder test");

#if DISPLAY_DRIVER_ST7789
  // Common sizes: 240x240, 240x280, 240x320.
  // If the image is clipped or offset, try another size here.
  tft.init(240, 320);
  tft.setRotation(1);
#else
  tft.begin();
  tft.setRotation(1);
#endif

  screenW = tft.width();
  screenH = tft.height();
  drawScreen();
}

void loop()
{
  bool buttonNow = digitalRead(ENC_SW);
  noInterrupts();
  static int32_t lastCount = 0;
  int32_t countNow = encoderCount;
  interrupts();

  if (countNow != lastCount || buttonNow != buttonLast ||
      millis() - lastRedrawMs > 1000)
  {
    lastCount = countNow;
    buttonLast = buttonNow;
    lastRedrawMs = millis();
    drawScreen();

    Serial.print("encoder=");
    Serial.print(countNow);
    Serial.print(" button=");
    Serial.println(buttonNow == LOW ? "DOWN" : "UP");
  }
}
