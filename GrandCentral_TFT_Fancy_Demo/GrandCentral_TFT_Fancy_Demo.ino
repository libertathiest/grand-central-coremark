/*
  Grand Central M4 - fancy SPI TFT demo for BioResonancePro.

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

  Default display driver is ST7789. If your display is ILI9341, change
  DISPLAY_DRIVER_ST7789 to 0 and DISPLAY_DRIVER_ILI9341 to 1.
*/

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>

#define DISPLAY_DRIVER_ST7789  1
#define DISPLAY_DRIVER_ILI9341 0

#if DISPLAY_DRIVER_ST7789
#include <Adafruit_ST7789.h>
#define TFT_BLACK ST77XX_BLACK
#define TFT_WHITE ST77XX_WHITE
#elif DISPLAY_DRIVER_ILI9341
#include <Adafruit_ILI9341.h>
#define TFT_BLACK ILI9341_BLACK
#define TFT_WHITE ILI9341_WHITE
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

uint16_t W = 0;
uint16_t H = 0;
uint32_t frame = 0;
uint32_t lastFrameMs = 0;
uint16_t baseFreqHz = 432;
uint8_t theme = 0;
bool buttonWasDown = false;

uint16_t rgb(uint8_t r, uint8_t g, uint8_t b)
{
  return tft.color565(r, g, b);
}

uint16_t wheel(uint8_t pos)
{
  pos = 255 - pos;
  if (pos < 85)
    return rgb(255 - pos * 3, 0, pos * 3);
  if (pos < 170)
  {
    pos -= 85;
    return rgb(0, pos * 3, 255 - pos * 3);
  }
  pos -= 170;
  return rgb(pos * 3, 255 - pos * 3, 0);
}

void encoderIsr()
{
  uint8_t a = digitalRead(ENC_A);
  uint8_t b = digitalRead(ENC_B);
  uint8_t curr = (a << 1) | b;
  uint8_t transition = (encoderPrev << 2) | curr;

  if (transition == 0b0001 || transition == 0b0111 ||
      transition == 0b1110 || transition == 0b1000)
    encoderCount++;
  else if (transition == 0b0010 || transition == 0b1011 ||
           transition == 0b1101 || transition == 0b0100)
    encoderCount--;

  encoderPrev = curr;
}

void splash()
{
  tft.fillScreen(TFT_BLACK);
  for (int r = 4; r < min(W, H) / 2; r += 5)
  {
    tft.drawCircle(W / 2, H / 2, r, wheel(r * 4));
    delay(12);
  }

  tft.setTextWrap(false);
  tft.setTextSize(3);
  tft.setTextColor(TFT_WHITE);
  int16_t x1, y1;
  uint16_t tw, th;
  tft.getTextBounds("BIO", 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((W - tw) / 2, H / 2 - 28);
  tft.print("BIO");

  tft.setTextSize(2);
  tft.getTextBounds("RESONANCE PRO", 0, 0, &x1, &y1, &tw, &th);
  tft.setCursor((W - tw) / 2, H / 2 + 8);
  tft.print("RESONANCE PRO");
  delay(750);
}

void drawHeader(uint16_t freq)
{
  tft.fillRect(0, 0, W, 30, rgb(8, 12, 18));
  tft.drawFastHLine(0, 30, W, wheel(frame * 3));
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE);
  tft.setCursor(8, 7);
  tft.print("BioResonance");

  tft.setTextSize(1);
  tft.setTextColor(rgb(145, 220, 255));
  tft.setCursor(W - 86, 5);
  tft.print(freq);
  tft.print(" Hz");
  tft.setCursor(W - 86, 17);
  tft.print("TFT DEMO");
}

void drawFrequencyPanel(uint16_t freq)
{
  int x = 8;
  int y = 42;
  int w = W / 2 - 14;
  int h = 72;

  tft.fillRoundRect(x, y, w, h, 6, rgb(12, 22, 34));
  tft.drawRoundRect(x, y, w, h, 6, rgb(52, 110, 150));
  tft.setTextColor(rgb(130, 210, 255));
  tft.setTextSize(1);
  tft.setCursor(x + 9, y + 8);
  tft.print("FREQUENCY");

  tft.setTextColor(TFT_WHITE);
  tft.setTextSize(3);
  tft.setCursor(x + 12, y + 28);
  tft.print(freq);

  tft.setTextSize(1);
  tft.setCursor(x + 12, y + 58);
  tft.print("30 - 2000 Hz sweep");
}

void drawPowerPanel(uint16_t level)
{
  int x = W / 2 + 6;
  int y = 42;
  int w = W / 2 - 14;
  int h = 72;

  tft.fillRoundRect(x, y, w, h, 6, rgb(18, 16, 28));
  tft.drawRoundRect(x, y, w, h, 6, rgb(120, 75, 190));
  tft.setTextSize(1);
  tft.setTextColor(rgb(220, 190, 255));
  tft.setCursor(x + 9, y + 8);
  tft.print("OUTPUT POWER");

  int barX = x + 12;
  int barY = y + 32;
  int barW = w - 24;
  int fill = map(level, 0, 100, 0, barW);
  tft.drawRect(barX, barY, barW, 14, rgb(90, 90, 120));
  for (int i = 0; i < fill; i++)
    tft.drawFastVLine(barX + i, barY + 1, 12, wheel(i * 3 + frame * 4));

  tft.setTextColor(TFT_WHITE);
  tft.setCursor(x + 12, y + 54);
  tft.print(level);
  tft.print("% simulated feedback");
}

void drawWaveform(uint16_t freq)
{
  int x = 8;
  int y = 126;
  int w = W - 16;
  int h = 78;
  int cy = y + h / 2;

  tft.fillRoundRect(x, y, w, h, 6, rgb(5, 8, 12));
  tft.drawRoundRect(x, y, w, h, 6, rgb(48, 88, 92));
  tft.drawFastHLine(x + 6, cy, w - 12, rgb(35, 45, 55));

  float phase = frame * 0.18f;
  int lastY = cy;
  for (int i = 0; i < w - 14; i++)
  {
    float a = (i * 0.075f) + phase;
    float carrier = sinf(a);
    float shimmer = 0.28f * sinf(a * 3.0f + frame * 0.05f);
    int yy = cy + (int)((carrier + shimmer) * (h * 0.30f));
    uint16_t c = wheel((i * 2 + frame * 5 + theme * 48) & 0xFF);
    if (i > 0)
      tft.drawLine(x + 7 + i - 1, lastY, x + 7 + i, yy, c);
    lastY = yy;
  }

  tft.setTextSize(1);
  tft.setTextColor(rgb(160, 225, 220));
  tft.setCursor(x + 10, y + 8);
  tft.print("DDS waveform monitor");
  tft.setCursor(x + w - 88, y + 8);
  tft.print(freq);
  tft.print(" Hz");
}

void drawOrbit()
{
  int cx = W / 2;
  int cy = H - 48;
  int radius = min(W, H) / 8;

  tft.fillCircle(cx, cy, radius + 18, rgb(0, 0, 0));
  tft.drawCircle(cx, cy, radius + 16, rgb(28, 45, 62));
  tft.drawCircle(cx, cy, radius + 8, rgb(20, 32, 45));

  for (int i = 0; i < 10; i++)
  {
    float a = frame * 0.07f + i * 0.628f;
    float b = frame * 0.045f + i * 0.9f;
    int px = cx + (int)(cosf(a) * (radius + 12));
    int py = cy + (int)(sinf(b) * (radius + 2));
    tft.fillCircle(px, py, 3 + (i % 3), wheel(i * 25 + frame * 3));
  }

  tft.setTextSize(1);
  tft.setTextColor(rgb(160, 160, 175));
  tft.setCursor(8, H - 18);
  tft.print("Encoder will tune frequency when connected");
}

void drawFrame()
{
  noInterrupts();
  int32_t enc = encoderCount;
  interrupts();

  int32_t tuned = (int32_t)baseFreqHz + enc * 2;
  if (tuned < 30)
    tuned = 30;
  if (tuned > 2000)
    tuned = 2000;

  uint16_t sweep = 30 + (uint16_t)((sinf(frame * 0.035f) + 1.0f) * 985.0f);
  uint16_t freq = enc == 0 ? sweep : (uint16_t)tuned;
  uint16_t power = 12 + (uint16_t)((sinf(frame * 0.09f) + 1.0f) * 40.0f);

  tft.fillScreen(TFT_BLACK);
  drawHeader(freq);
  drawFrequencyPanel(freq);
  drawPowerPanel(power);
  drawWaveform(freq);
  drawOrbit();
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
  Serial.println("Grand Central M4 fancy TFT demo");

#if DISPLAY_DRIVER_ST7789
  tft.init(240, 320);
  tft.setRotation(1);
#else
  tft.begin();
  tft.setRotation(1);
#endif

  W = tft.width();
  H = tft.height();
  splash();
}

void loop()
{
  bool buttonDown = digitalRead(ENC_SW) == LOW;
  if (buttonDown && !buttonWasDown)
    theme++;
  buttonWasDown = buttonDown;

  if (millis() - lastFrameMs >= 45)
  {
    lastFrameMs = millis();
    frame++;
    drawFrame();
  }
}
