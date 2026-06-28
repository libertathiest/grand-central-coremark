/*
  Grand Central M4 - benchmark menu with rotary encoder + OLED.

  Rotate the encoder to highlight a menu item, press the switch to launch it.
  While a benchmark is running, press the switch again to return to the menu.

  Menu items:
    1. Prime Number Gen  - continuous prime search (trial division)
    2. CPU Load Monitor  - FPU workload loop-rate / timing dashboard
    3. BioResonance Pro  - full preset/frequency menu (Tuning Fork,
                            Vertebra, Organs, Chakra), same UI as the
                            standalone BioResonance Grand Central sketch.
    4. Read Files        - browse the onboard QSPI flash filesystem;
                            Prime Gen and CPU Monitor append their stats
                            to /benchmarks.log, and BioResonance Pro logs
                            preset selections / output toggles to
                            /usage.log, so this mode has real data to
                            read back.

  Wiring:
    OLED (I2C, SSD1306 128x64 @ 0x3C):
      VCC -> 3.3V, GND -> GND, SDA -> SDA (20), SCL -> SCL (21)
    Encoder:
      ENC_A  -> D2
      ENC_B  -> D3
      ENC_SW -> D4 (active low, internal pull-up)
    BioResonance Pro extras:
      OUTPUT_SW (front-panel output toggle) -> D5 (active low, internal pull-up)
      OUTPUT_ENABLE (amp/DDS enable, active high) -> D6
      Digital volume control: MCP4551 I2C digital potentiometer on the
      same I2C bus as the OLED (SDA/SCL), address 0x2F.
      AD9833 DDS (HiLetGo module), SPI:
        VCC -> 3.3V, GND -> GND
        FSYNC -> D10, SCLK -> SCK, SDATA -> MOSI
      Speaker feedback (per daughterboard schematic):
        AMP_LEVEL_ADC -> A0 (relative/uncalibrated output-level readback)
        AMP_CURRENT_ADC -> A1 (reserved for future current sensor, unconnected)
*/

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include "SdFat_Adafruit_Fork.h"
#include <Adafruit_SPIFlash.h>
#include <Adafruit_TinyUSB.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSerif18pt7b.h>

#define TFT_BLACK ST77XX_BLACK
#define TFT_WHITE ST77XX_WHITE

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240
#define TFT_CS        9
#define TFT_DC        8
#define TFT_RST       7
#define TFT_BL        11

#define ENC_A  2
#define ENC_B  3
#define ENC_SW 4

#define BIO_OUTPUT_SW     5  // BioResonance Pro: front-panel output toggle
#define BIO_OUTPUT_ENABLE 6  // BioResonance Pro: amp/DDS enable (active high)

// BioResonance Pro: MCP4551 I2C digital potentiometer (volume control),
// shares the OLED's I2C bus.
#define MCP4551_I2C_ADDR  0x2F
#define MCP4551_WIPER_CMD 0x00 // "write data" command to the wiper register

// BioResonance Pro: speaker feedback - AMP_LEVEL_ADC is a relative,
// uncalibrated readback of the delivered output level (per schematic,
// Grand Central A0). AMP_CURRENT_ADC (A1) is reserved for a future real
// current sensor and is left unconnected for now.
#define AMP_LEVEL_ADC A0

Adafruit_ST7789 display(&SPI, TFT_CS, TFT_DC, TFT_RST);

void tftFlush()
{
    // Adafruit_ST7789 draws immediately; this keeps old OLED code readable.
}

// ---------------------------------------------------------------------
// Onboard QSPI flash filesystem
// ---------------------------------------------------------------------
Adafruit_FlashTransport_QSPI flashTransport;
Adafruit_SPIFlash             flash(&flashTransport);
FatVolume                      fatfs;
bool                            fsReady = false;

// USB Drive mode (declared here so appendLog() can see it; defined in the
// "USB Drive mode" section below).
bool usbDriveActive = false;

const char *LOG_FILE   = "/benchmarks.log";
const char *USAGE_FILE = "/usage.log";
const char *STARTUP_APP_FILE = "/startup_app.txt";

// Cap each log file at this size. Once a log reaches the cap it is renamed
// to "<path>.old" (replacing any previous .old) and a fresh file is started,
// so /benchmarks.log + /usage.log can't slowly fill the 8.4MB flash.
const uint32_t LOG_MAX_BYTES = 256UL * 1024;

// Renames `path` to `path.old` if it has reached LOG_MAX_BYTES.
void rotateLogIfFull(const char *path)
{
    File32 f = fatfs.open(path, FILE_READ);
    if (!f)
        return;
    uint32_t size = f.size();
    f.close();
    if (size < LOG_MAX_BYTES)
        return;

    char oldPath[40];
    snprintf(oldPath, sizeof(oldPath), "%s.old", path);
    fatfs.remove(oldPath);
    fatfs.rename(path, oldPath);
}

// Appends one line to the given file (no-op if the filesystem isn't ready,
// or while the USB Drive menu has handed the flash to a connected
// computer).
void appendLog(const char *path, const char *line)
{
    if (!fsReady || usbDriveActive)
        return;
    rotateLogIfFull(path);
    File32 f = fatfs.open(path, FILE_WRITE);
    if (f)
    {
        f.println(line);
        f.close();
    }
}

void logBenchmark(const char *line)
{
    appendLog(LOG_FILE, line);
}

// ---------------------------------------------------------------------
// USB Drive mode - exposes the QSPI flash filesystem as a USB mass
// storage drive so a customer can plug the unit into a computer and
// read /benchmarks.log and /usage.log directly with a text editor.
// While active, the firmware stops touching the flash filesystem itself
// (see appendLog()) so the host's view of the FAT volume stays valid.
// ---------------------------------------------------------------------
Adafruit_USBD_MSC usb_msc;

// Callback invoked when the host sends a READ10 command.
int32_t mscReadCallback(uint32_t lba, void *buffer, uint32_t bufsize)
{
    return flash.readBlocks(lba, (uint8_t *)buffer, bufsize / 512) ? bufsize : -1;
}

// Callback invoked when the host sends a WRITE10 command.
int32_t mscWriteCallback(uint32_t lba, uint8_t *buffer, uint32_t bufsize)
{
    return flash.writeBlocks(lba, buffer, bufsize / 512) ? bufsize : -1;
}

// Callback invoked once a WRITE10 command has been fully accepted -
// flush the flash's block cache and SdFat's volume cache.
void mscFlushCallback()
{
    flash.syncBlocks();
    fatfs.cacheClear();
}

void usbDriveEnter()
{
    usbDriveActive = true;
    flash.syncBlocks();
    fatfs.cacheClear();
    usb_msc.setUnitReady(true);

    display.fillScreen(TFT_BLACK);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(0, 0);
    display.println("USB Drive Mode");
    display.println();
    display.println("Connect to a computer");
    display.println("to view:");
    display.println(" /benchmarks.log");
    display.println(" /usage.log");
    display.println();
    display.println("Eject, then press");
    display.println("to exit.");
    tftFlush();
}

void usbDriveExit()
{
    usb_msc.setUnitReady(false);
    flash.syncBlocks();
    fatfs.cacheClear();
    usbDriveActive = false;
    fsReady         = fatfs.begin(&flash);
}

void usbDriveLoop()
{
    // Static screen; TinyUSB services MSC transfers in the background.
}

// ---------------------------------------------------------------------
// Encoder (quadrature decode via interrupts)
// ---------------------------------------------------------------------
volatile int8_t  encDelta = 0;
volatile uint8_t encPrevState = 0;
volatile int8_t  encAccum = 0;

// Table indexed by (prevAB << 2 | currAB) -> -1, 0, or +1
const int8_t encTable[16]
    = { 0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0 };

// Requires a full 4-transition quadrature cycle back to the detent
// position (A=B=0) before counting a step. This rejects the partial,
// back-and-forth transitions caused by contact bounce, which otherwise
// show up as jittery/double menu movement.
void encoderISR()
{
    uint8_t a = digitalRead(ENC_A);
    uint8_t b = digitalRead(ENC_B);
    uint8_t curr = (a << 1) | b;
    uint8_t idx  = (encPrevState << 2) | curr;
    encAccum += encTable[idx & 0x0F];
    encPrevState = curr;

    if (curr == 0)
    {
        if (encAccum >= 4)
            encDelta++;
        else if (encAccum <= -4)
            encDelta--;
        encAccum = 0;
    }
}

// Debounced ENC_SW state, shared by the top-level menu and BioResonance Pro
// (which needs both press and release edges to distinguish short vs. long
// presses). Call once per loop() iteration.
bool          encSwHeld       = false; // true while button is held (LOW)
bool          encSwPressEdge  = false; // true for one loop after a press
bool          encSwReleaseEdge = false; // true for one loop after a release
unsigned long encSwLastChange = 0;

void updateEncSw()
{
    bool pressed = (digitalRead(ENC_SW) == LOW);
    encSwPressEdge   = false;
    encSwReleaseEdge = false;
    if (pressed != encSwHeld && millis() - encSwLastChange > 100)
    {
        encSwLastChange = millis();
        encSwHeld       = pressed;
        if (pressed)
            encSwPressEdge = true;
        else
            encSwReleaseEdge = true;
    }
}

// ---------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------
const char *menuItems[] = { "Prime Number Gen", "CPU Load Monitor",
                             "BioResonance Pro", "Read Files",
                             "USB Drive Mode" };
const int NUM_ITEMS = 5;
int        menuIndex = 0;
int        startupMenuIndex = 2; // Default product mode: BioResonance Pro.
bool       serviceMenuVisible = false;
uint32_t   serviceMenuHoldStartMs = 0;
bool       serviceMenuComboWasHeld = false;

enum AppState
{
    STATE_MENU,
    STATE_PRIMES,
    STATE_CPU,
    STATE_FILES,
    STATE_BIORESONANCE,
    STATE_USB_DRIVE
};
AppState appState = STATE_MENU;

int stateForMenuIndex(int index)
{
    switch (index)
    {
        case 0:
            return STATE_PRIMES;
        case 1:
            return STATE_CPU;
        case 2:
            return STATE_BIORESONANCE;
        case 3:
            return STATE_FILES;
        case 4:
            return STATE_USB_DRIVE;
        default:
            return STATE_BIORESONANCE;
    }
}

void saveStartupApp()
{
    if (!fsReady || usbDriveActive)
        return;

    File32 f = fatfs.open(STARTUP_APP_FILE, O_WRITE | O_CREAT | O_TRUNC);
    if (!f)
        return;
    f.println(startupMenuIndex);
    f.close();
}

void loadStartupApp()
{
    startupMenuIndex = 2;
    if (!fsReady)
        return;

    File32 f = fatfs.open(STARTUP_APP_FILE, FILE_READ);
    if (!f)
        return;

    char buf[8] = { 0 };
    int  n      = f.read(buf, sizeof(buf) - 1);
    f.close();
    if (n <= 0)
        return;

    int index = atoi(buf);
    if (index >= 0 && index < NUM_ITEMS)
        startupMenuIndex = index;
}

void drawMenu()
{
    display.fillScreen(TFT_BLACK);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(0, 0);
    display.println("GC M4 Benchmark Menu");

    for (int i = 0; i < NUM_ITEMS; i++)
    {
        display.setCursor(0, 12 + i * 10);
        display.print(i == menuIndex ? "> " : "  ");
        display.println(menuItems[i]);
    }
    tftFlush();
}

// ---------------------------------------------------------------------
// 1. Prime number generator
// ---------------------------------------------------------------------
unsigned long primeCandidate, lastPrime, primeCount, primesInWindow,
    primeWindowStart;

void primesEnter()
{
    primeCandidate   = 1;
    lastPrime        = 0;
    primeCount       = 0;
    primesInWindow   = 0;
    primeWindowStart = millis();

    display.fillScreen(TFT_BLACK);
    display.setCursor(0, 0);
    display.println("Prime Number Gen");
    display.println();
    display.println("Starting...");
    display.println();
    display.println("Press to exit");
    tftFlush();
}

bool isPrime(unsigned long n)
{
    if (n < 2)
        return false;
    if (n % 2 == 0)
        return n == 2;
    for (unsigned long i = 3; i * i <= n; i += 2)
    {
        if (n % i == 0)
            return false;
    }
    return true;
}

void primesLoop()
{
    primeCandidate++;
    if (isPrime(primeCandidate))
    {
        lastPrime = primeCandidate;
        primeCount++;
        primesInWindow++;
    }

    unsigned long now     = millis();
    unsigned long elapsed = now - primeWindowStart;
    if (elapsed >= 1000)
    {
        float rate = (primesInWindow * 1000.0f) / elapsed;

        display.fillScreen(TFT_BLACK);
        display.setCursor(0, 0);
        display.println("Prime Number Gen");
        display.print("Found: ");
        display.println(primeCount);
        display.print("Last : ");
        display.println(lastPrime);
        display.print("Rate : ");
        display.print(rate, 1);
        display.println(" /s");
        display.print("Testing: ");
        display.println(primeCandidate);
        display.println();
        display.println("Press to exit");
        tftFlush();

        char logLine[64];
        snprintf(logLine, sizeof(logLine),
                 "[%lus] primes: found=%lu last=%lu rate=%.1f/s", now / 1000,
                 primeCount, lastPrime, rate);
        logBenchmark(logLine);

        primeWindowStart = now;
        primesInWindow   = 0;
    }
}

// ---------------------------------------------------------------------
// 2. CPU load monitor
// ---------------------------------------------------------------------
unsigned long cpuWindowStart, cpuLoopCount, cpuSumUs, cpuMinUs, cpuMaxUs;

extern "C" char *sbrk(int i);
int freeMemory()
{
    char stack_dummy = 0;
    return &stack_dummy - sbrk(0);
}

void cpuEnter()
{
    cpuWindowStart = millis();
    cpuLoopCount   = 0;
    cpuSumUs       = 0;
    cpuMinUs       = 0xFFFFFFFF;
    cpuMaxUs       = 0;

    display.fillScreen(TFT_BLACK);
    display.setCursor(0, 0);
    display.println("CPU Load Monitor");
    display.println();
    display.println("Starting...");
    display.println();
    display.println("Press to exit");
    tftFlush();
}

void cpuDoWork()
{
    volatile float x = 1.0001f;
    for (int i = 0; i < 2000; i++)
    {
        x = sinf(x) * cosf(x) + sqrtf(x);
    }
}

void cpuLoop()
{
    unsigned long t0 = micros();
    cpuDoWork();
    unsigned long t1 = micros();
    unsigned long dt = t1 - t0;

    cpuLoopCount++;
    cpuSumUs += dt;
    if (dt < cpuMinUs)
        cpuMinUs = dt;
    if (dt > cpuMaxUs)
        cpuMaxUs = dt;

    unsigned long now     = millis();
    unsigned long elapsed = now - cpuWindowStart;
    if (elapsed >= 1000)
    {
        float loopsPerSec = (cpuLoopCount * 1000.0f) / elapsed;
        float avgUs       = (float)cpuSumUs / cpuLoopCount;

        display.fillScreen(TFT_BLACK);
        display.setCursor(0, 0);
        display.println("CPU Load Monitor");
        display.print("Loop: ");
        display.print(loopsPerSec, 1);
        display.println(" Hz");
        display.print("Avg : ");
        display.print(avgUs, 1);
        display.println(" us");
        display.print("Min/Max: ");
        display.print(cpuMinUs);
        display.print("/");
        display.println(cpuMaxUs);
        display.print("Free RAM: ");
        display.print(freeMemory() / 1024);
        display.println(" KB");
        display.println("Press to exit");
        tftFlush();

        char logLine[64];
        snprintf(logLine, sizeof(logLine),
                 "[%lus] cpu: %.1fHz avg=%.1fus min=%lu max=%lu", now / 1000,
                 loopsPerSec, avgUs, cpuMinUs, cpuMaxUs);
        logBenchmark(logLine);

        cpuWindowStart = now;
        cpuLoopCount   = 0;
        cpuSumUs       = 0;
        cpuMinUs       = 0xFFFFFFFF;
        cpuMaxUs       = 0;
    }
}

// ---------------------------------------------------------------------
// 4. Read Files - browse the QSPI flash filesystem (scrollable)
// ---------------------------------------------------------------------
const int  FILES_VISIBLE_LINES = 6;
const int  MAX_FILE_LINES       = 48;
const int  FILE_LINE_WIDTH      = 21; // chars, leaves room for scrollbar
char       fileLines[MAX_FILE_LINES][FILE_LINE_WIDTH];
int        fileLineCount    = 0;
int        fileScrollOffset = 0;

void addFileLine(const char *s)
{
    if (fileLineCount >= MAX_FILE_LINES)
        return;
    strncpy(fileLines[fileLineCount], s, FILE_LINE_WIDTH - 1);
    fileLines[fileLineCount][FILE_LINE_WIDTH - 1] = '\0';
    fileLineCount++;
}

// Loads a "<path> <size>B" header line followed by the most recent
// `maxLines` non-empty lines of the file into the fileLines buffer (or a
// single placeholder if missing). Reads only a bounded chunk from the end
// of the file (via seek) and splits it in place - streaming a multi-MB
// log through String::readStringUntil() took minutes on SAMD51.
const uint32_t FILE_TAIL_BYTES = 2048;

void loadFileLines(const char *path, int maxLines)
{
    char  header[FILE_LINE_WIDTH];
    File32 f = fatfs.open(path);
    if (!f)
    {
        snprintf(header, sizeof(header), "%s (empty)", path);
        addFileLine(header);
        return;
    }

    uint32_t fileSize = f.size();
    snprintf(header, sizeof(header), "%s %luB", path, (unsigned long)fileSize);
    addFileLine(header);

    if (maxLines > 24)
        maxLines = 24;

    uint32_t readSize = min(fileSize, FILE_TAIL_BYTES);
    uint32_t seekPos  = fileSize - readSize;

    static char tail[FILE_TAIL_BYTES + 1];
    f.seekSet(seekPos);
    int n = f.read(tail, readSize);
    f.close();
    if (n < 0)
        n = 0;
    tail[n] = '\0';

    // The tail likely starts mid-line unless we read from byte 0 -
    // drop that leading partial line.
    char *line = tail;
    if (seekPos > 0)
    {
        char *nl = strchr(tail, '\n');
        line = nl ? nl + 1 : tail + n;
    }

    static char ring[24][FILE_LINE_WIDTH];
    int total = 0, head = 0;
    while (*line)
    {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';

        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\r')
            line[--len] = '\0';

        if (len > 0)
        {
            strncpy(ring[head % maxLines], line, FILE_LINE_WIDTH - 1);
            ring[head % maxLines][FILE_LINE_WIDTH - 1] = '\0';
            head++;
            total++;
        }

        if (!nl)
            break;
        line = nl + 1;
    }

    int count = min(total, maxLines);
    int start = (total > maxLines) ? (head % maxLines) : 0;
    for (int i = 0; i < count && fileLineCount < MAX_FILE_LINES; i++)
        addFileLine(ring[(start + i) % maxLines]);
}

void filesDraw()
{
    display.fillScreen(TFT_BLACK);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(0, 0);
    display.println("Flash File Browser");

    for (int i = 0; i < FILES_VISIBLE_LINES; i++)
    {
        int idx = fileScrollOffset + i;
        if (idx >= fileLineCount)
            break;
        display.setCursor(0, 8 + i * 8);
        display.print(fileLines[idx]);
    }

    if (fileLineCount > FILES_VISIBLE_LINES)
    {
        display.drawRect(125, 8, 3, 48, TFT_WHITE);
        int maxOffset = fileLineCount - FILES_VISIBLE_LINES;
        int thumbH    = max(4, 48 * FILES_VISIBLE_LINES / fileLineCount);
        int thumbY    = 8 + ((48 - thumbH) * fileScrollOffset) / maxOffset;
        display.fillRect(126, thumbY, 1, thumbH, TFT_WHITE);
    }

    display.setCursor(0, 56);
    display.print("Press to exit");
    tftFlush();
}

void filesEnter()
{
    fileLineCount    = 0;
    fileScrollOffset = 0;

    if (!fsReady)
    {
        addFileLine("No filesystem found.");
        addFileLine("(QSPI flash needs to");
        addFileLine(" be formatted first)");
    }
    else
    {
        loadFileLines(LOG_FILE, 23);
        loadFileLines(USAGE_FILE, 23);
    }

    filesDraw();
}

// Scroll the file view by `delta` lines (called from the encoder handler).
void filesScroll(int8_t delta)
{
    int maxOffset
        = fileLineCount > FILES_VISIBLE_LINES ? fileLineCount - FILES_VISIBLE_LINES : 0;
    fileScrollOffset += delta;
    if (fileScrollOffset < 0)
        fileScrollOffset = 0;
    if (fileScrollOffset > maxOffset)
        fileScrollOffset = maxOffset;
    filesDraw();
}

void filesLoop()
{
    // Static view; redrawn only on scroll (see filesScroll).
}

// ---------------------------------------------------------------------
// AD9833 DDS driver (HiLetGo module) - SPI, FSYNC on D10, SCK/MOSI shared
// with the main SPI bus (SDATA -> MOSI, SCLK -> SCK). Output amplitude is
// handled separately by the MCP4551 + OUTPUT_ENABLE; this driver just
// programs the sine-wave frequency and powers the DAC down when idle.
// ---------------------------------------------------------------------
#define AD9833_FSYNC 10
const uint32_t AD9833_MCLK_HZ = 25000000UL; // HiLetGo module's onboard crystal

const uint16_t AD9833_B28    = 0x2000;
const uint16_t AD9833_RESET  = 0x0100;
const uint16_t AD9833_SLEEP1 = 0x0080; // power down internal DAC
const uint16_t AD9833_SLEEP12 = 0x0040; // disable internal clock
const uint16_t AD9833_FREQ0_WRITE = 0x4000;

const SPISettings AD9833_SPI_SETTINGS(2000000, MSBFIRST, SPI_MODE2);

void ad9833WriteRegister(uint16_t data)
{
    digitalWrite(AD9833_FSYNC, LOW);
    SPI.transfer(data >> 8);
    SPI.transfer(data & 0xFF);
    digitalWrite(AD9833_FSYNC, HIGH);
}

void ad9833Init()
{
    pinMode(AD9833_FSYNC, OUTPUT);
    digitalWrite(AD9833_FSYNC, HIGH);
    SPI.begin();

    SPI.beginTransaction(AD9833_SPI_SETTINGS);
    ad9833WriteRegister(AD9833_B28 | AD9833_RESET);
    SPI.endTransaction();
}

// Programs the sine-wave output frequency (Hz). Leaves the device in
// reset if it was already in reset (use ad9833SetSleep() to start it).
void ad9833SetFrequency(uint32_t freqHz)
{
    uint32_t freqWord
        = (uint32_t)(((uint64_t)freqHz << 28) / AD9833_MCLK_HZ);
    uint16_t lsb = (uint16_t)(freqWord & 0x3FFF) | AD9833_FREQ0_WRITE;
    uint16_t msb = (uint16_t)((freqWord >> 14) & 0x3FFF) | AD9833_FREQ0_WRITE;

    SPI.beginTransaction(AD9833_SPI_SETTINGS);
    ad9833WriteRegister(AD9833_B28 | AD9833_RESET);
    ad9833WriteRegister(lsb);
    ad9833WriteRegister(msb);
    ad9833WriteRegister(AD9833_B28);
    SPI.endTransaction();
}

// Powers the DAC/clock down (true) or starts normal sine output (false).
void ad9833SetSleep(bool sleep)
{
    SPI.beginTransaction(AD9833_SPI_SETTINGS);
    ad9833WriteRegister(AD9833_B28 | (sleep ? AD9833_SLEEP1 | AD9833_SLEEP12
                                             : 0));
    SPI.endTransaction();
}

// ---------------------------------------------------------------------
// 3. BioResonance Pro - preset / frequency menu (ported from the
//    standalone menu_bioresonance_grandcentral sketch, redrawn with
//    Adafruit_GFX instead of U8g2 so it shares the OLED with the rest
//    of this menu).
// ---------------------------------------------------------------------
const uint16_t BIO_MIN_FREQ_HZ     = 30;
const uint16_t BIO_MAX_FREQ_HZ     = 2000;
const uint16_t BIO_DEFAULT_FREQ_HZ = 528;

struct BioPreset
{
    const char *name;
    uint16_t    hz;
};

struct BioCategory
{
    const char       *name;
    const BioPreset *items;
    uint8_t           count;
};

const BioPreset bioTuningForkPresets[] = {
    { "Lymphatic", 32 },     { "Muscles", 64 }, { "Musc. Skel.", 128 },
    { "Solfeggio", 174 },    { "Chakra", 432 },
};

const BioPreset bioVertebraPresets[] = {
    { "C1/T1", 64 },  { "C2/T2", 72 },   { "C3/T3", 80 },   { "C4/T4", 84 },
    { "C5/T5", 97 },  { "C6/T6", 108 },  { "C7/T7", 120 },  { "T8/L1", 68 },
    { "T9/L2", 76 },  { "T10/L3", 88 },  { "T11/L4", 100 }, { "T12/L5", 116 },
};

const BioPreset bioOrganPresets[] = {
    { "Heart", 246 },    { "Adrenals", 492 }, { "Pancreas", 117 },
    { "Urinary B", 352 }, { "Kidney", 320 },   { "Stomach", 110 },
    { "Gall Bltr", 164 }, { "Liver", 233 },    { "Large Int", 176 },
    { "Lungs", 220 },     { "Small Int", 281 },
};

const BioPreset bioChakraPresets[] = {
    { "Crown", 768 },     { "Third Eye", 720 }, { "Root", 432 },
    { "Sacral", 480 },    { "Solar Plex", 528 }, { "Heart", 594 },
    { "Throat", 672 },
};

const BioCategory bioCategories[] = {
    { "Tuning Fork", bioTuningForkPresets,
      sizeof(bioTuningForkPresets) / sizeof(bioTuningForkPresets[0]) },
    { "Vertebra", bioVertebraPresets,
      sizeof(bioVertebraPresets) / sizeof(bioVertebraPresets[0]) },
    { "Organs", bioOrganPresets,
      sizeof(bioOrganPresets) / sizeof(bioOrganPresets[0]) },
    { "Chakra", bioChakraPresets,
      sizeof(bioChakraPresets) / sizeof(bioChakraPresets[0]) },
};
const uint8_t BIO_CATEGORY_COUNT
    = sizeof(bioCategories) / sizeof(bioCategories[0]);

enum BioMode : uint8_t
{
    BIO_MAIN,
    BIO_PRESET,
    BIO_FREQUENCY,
    BIO_VOLUME
};

const uint8_t BIO_VOLUME_MIN        = 0;
const uint8_t BIO_VOLUME_MAX        = 100;
const uint8_t BIO_VOLUME_STEP       = 5;
const uint8_t BIO_VOLUME_DEFAULT    = 80;
const uint8_t BIO_VOLUME_SAFE_START = 10; // OFF->ON always starts here to avoid overdriving the speaker

BioMode  bioMode             = BIO_MAIN;
uint8_t  bioSelectedCategory = 0;
uint8_t  bioSelectedPreset[BIO_CATEGORY_COUNT] = { 0 };
uint16_t bioFrequencyHz      = BIO_DEFAULT_FREQ_HZ;
uint16_t bioActivePresetHz   = BIO_DEFAULT_FREQ_HZ;
bool     bioOutputEnabled    = false;
uint8_t  bioVolume           = BIO_VOLUME_DEFAULT;
uint32_t bioLastFreqTurnMs   = 0;
uint8_t  bioLastStepSize     = 1;
uint32_t bioButtonDownMs     = 0;
bool     bioButtonWasHeld    = false;
uint16_t bioAmpLevel         = 0; // last AMP_LEVEL_ADC reading, 0-1023 (relative, uncalibrated)

// Samples the speaker feedback envelope (AMP_LEVEL_ADC, A0). Relative
// output-level indicator only - not yet calibrated to real watts.
void bioReadAmpLevel()
{
    bioAmpLevel = analogRead(AMP_LEVEL_ADC);
}

uint8_t bioWrappedIndex(int16_t index, uint8_t count)
{
    while (index < 0)
        index += count;
    while (index >= count)
        index -= count;
    return index;
}

uint16_t bioClampFrequency(int32_t hz)
{
    if (hz < BIO_MIN_FREQ_HZ)
        return BIO_MIN_FREQ_HZ;
    if (hz > BIO_MAX_FREQ_HZ)
        return BIO_MAX_FREQ_HZ;
    return (uint16_t)hz;
}

void bioActivateSelectedPreset()
{
    const BioCategory &cat = bioCategories[bioSelectedCategory];
    const BioPreset   &preset = cat.items[bioSelectedPreset[bioSelectedCategory]];
    bioActivePresetHz = bioClampFrequency(preset.hz);
    bioFrequencyHz    = bioActivePresetHz;
    ad9833SetFrequency(bioFrequencyHz);
}

// Drives OUTPUT_ENABLE + the AD9833 sleep bits, and logs ON/OFF
// transitions to /usage.log.
void bioSetOutputEnabled(bool enabled)
{
    bool wasEnabled  = bioOutputEnabled;
    bioOutputEnabled = enabled && bioMode != BIO_MAIN;
    digitalWrite(BIO_OUTPUT_ENABLE, bioOutputEnabled ? HIGH : LOW);
    ad9833SetSleep(!bioOutputEnabled);

    // Every OFF->ON transition starts at a safe, low volume so the speaker
    // is never overdriven by whatever level was left from last time.
    if (bioOutputEnabled && !wasEnabled)
    {
        bioVolume = BIO_VOLUME_SAFE_START;
        bioApplyVolume();
    }

    if (bioOutputEnabled != wasEnabled)
    {
        const BioCategory &cat = bioCategories[bioSelectedCategory];
        const BioPreset   &preset
            = cat.items[bioSelectedPreset[bioSelectedCategory]];
        char line[64];
        snprintf(line, sizeof(line), "[%lus] bio: %s/%s %uHz output %s",
                 millis() / 1000, cat.name, preset.name, bioFrequencyHz,
                 bioOutputEnabled ? "ON" : "OFF");
        appendLog(USAGE_FILE, line);
    }
}

// Drives the MCP4551 digital pot over I2C and logs the new level to
// /usage.log. bioVolume is 0-100; scaled to the pot's 0-255 wiper range.
void bioApplyVolume()
{
    uint8_t wiper = map(bioVolume, 0, 100, 0, 255);
    Wire.beginTransmission(MCP4551_I2C_ADDR);
    Wire.write(MCP4551_WIPER_CMD << 4);
    Wire.write(wiper);
    Wire.endTransmission();

    char line[48];
    snprintf(line, sizeof(line), "[%lus] bio: volume %u%%", millis() / 1000,
             bioVolume);
    appendLog(USAGE_FILE, line);
}

uint8_t bioFrequencyStep()
{
    uint32_t now = millis();
    uint32_t gap = now - bioLastFreqTurnMs;
    bioLastFreqTurnMs = now;

    if (gap < 80)
        return 50;
    if (gap < 160)
        return 20;
    if (gap < 280)
        return 10;
    if (gap < 440)
        return 5;
    return 1;
}

// ---- Drawing helpers (Adafruit_GFX) --------------------------------
void bioCenteredText(const char *s, int y, uint8_t scale = 1)
{
    display.setTextSize(scale);
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH - (int)w) / 2;
    if (x < 0)
        x = 0;
    display.setCursor(x, y);
    display.print(s);
}

// Animated sine wave shown in the gap next to the ON/OFF badge while
// output is enabled - a quick visual "signal is live" indicator.
// bioWavePhase is advanced each redraw by bioLoop() to animate it.
uint16_t bioWavePhase = 0;

void bioDrawWaveform(int x, int y, int w, int h)
{
    int midY  = y + h / 2;
    int amp   = h / 2 - 1;
    int prevY = midY;
    for (int i = 0; i <= w; i++)
    {
        float angle = (bioWavePhase + i * 18) * (PI / 180.0f);
        int   yy    = midY - (int)(amp * sinf(angle));
        if (i > 0)
            display.drawLine(x + i - 1, prevY, x + i, yy, TFT_WHITE);
        prevY = yy;
    }
}

// Thumb only - the track is the menu box's own border, so the scrollbar
// blends into the box and spans its full interior height (top to bottom).
void bioDrawScrollbar(uint8_t selected, uint8_t count)
{
    if (count == 0)
        return;
    const uint8_t trackY = 10, trackH = 53;
    uint8_t thumbH = max((uint8_t)6, (uint8_t)(trackH / count));
    uint8_t maxY   = trackY + trackH - thumbH;
    uint8_t y      = trackY;
    if (count > 1)
        y = trackY + ((uint16_t)selected * (maxY - trackY)) / (count - 1);
    display.fillRect(123, y, 4, thumbH, TFT_WHITE);
}

// Frame sits just under the title and runs to the screen's bottom-left
// edge; row text is left-aligned a few pixels in from the frame's left
// edge so neither the frame nor the text crowd each other.
void bioDrawMenuRows(const char *title, const char *prev, const char *current,
                      const char *next, uint8_t selected, uint8_t count,
                      bool showOutputBadge = false)
{
    display.fillScreen(TFT_BLACK);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setTextWrap(false);

    if (showOutputBadge)
    {
        // Title hugs the left edge; ON/OFF badge (matching the Frequency
        // screen) sits at the far right, with the animated waveform
        // filling the gap between them while output is enabled.
        display.setCursor(0, 0);
        display.print(title);

        const int badgeW = 22, badgeH = 8, badgeX = SCREEN_WIDTH - badgeW;
        if (bioOutputEnabled)
        {
            display.fillRect(badgeX, 0, badgeW, badgeH, TFT_WHITE);
            display.setTextColor(TFT_BLACK);
            display.setCursor(badgeX + 4, 0);
            display.print("ON");
            display.setTextColor(TFT_WHITE);
        }
        else
        {
            display.drawRect(badgeX, 0, badgeW, badgeH, TFT_WHITE);
            display.setCursor(badgeX + 2, 0);
            display.print("OFF");
        }

        int16_t  tx1, ty1;
        uint16_t tw, th;
        display.getTextBounds(title, 0, 0, &tx1, &ty1, &tw, &th);
        int waveX = (int)tw + 4;
        int waveW = badgeX - 2 - waveX;
        if (bioOutputEnabled && waveW > 4)
            bioDrawWaveform(waveX, 0, waveW, 8);
    }
    else
    {
        bioCenteredText(title, 0);
    }

    display.drawRect(0, 9, 128, 55, TFT_WHITE);

    display.setCursor(4, 14);
    display.print("  ");
    display.print(prev);

    display.setCursor(4, 30);
    display.print("> ");
    display.print(current);

    display.setCursor(4, 46);
    display.print("  ");
    display.print(next);

    bioDrawScrollbar(selected, count);
    tftFlush();
}

void bioDrawMainMenu()
{
    uint8_t prev = bioWrappedIndex(bioSelectedCategory - 1, BIO_CATEGORY_COUNT);
    uint8_t next = bioWrappedIndex(bioSelectedCategory + 1, BIO_CATEGORY_COUNT);
    bioDrawMenuRows("Main Menu", bioCategories[prev].name,
                     bioCategories[bioSelectedCategory].name,
                     bioCategories[next].name, bioSelectedCategory,
                     BIO_CATEGORY_COUNT);
}

void bioPresetLabel(uint8_t index, char *out, size_t outSize)
{
    const BioCategory &cat    = bioCategories[bioSelectedCategory];
    const BioPreset   &preset = cat.items[index];
    snprintf(out, outSize, "%s %uHz", preset.name, preset.hz);
}

void bioDrawPresetMenu()
{
    const BioCategory &cat     = bioCategories[bioSelectedCategory];
    uint8_t            current = bioSelectedPreset[bioSelectedCategory];
    uint8_t            prev    = bioWrappedIndex(current - 1, cat.count);
    uint8_t            next    = bioWrappedIndex(current + 1, cat.count);
    char prevBuf[24], currentBuf[24], nextBuf[24];
    bioPresetLabel(prev, prevBuf, sizeof(prevBuf));
    bioPresetLabel(current, currentBuf, sizeof(currentBuf));
    bioPresetLabel(next, nextBuf, sizeof(nextBuf));
    bioDrawMenuRows(cat.name, prevBuf, currentBuf, nextBuf, current, cat.count, true);
}

void bioDrawFrequencyMenu()
{
    display.fillScreen(TFT_BLACK);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(2, 0);
    display.print("BIORESONANCE");
    display.drawRect(0, 12, 128, 40, TFT_WHITE);

    char buf[8];
    snprintf(buf, sizeof(buf), "%u", bioFrequencyHz);

    // Crisp, proportional digits instead of the blocky scaled-up default
    // font - FreeSerif18pt7b digits are ~25px tall, comfortably inside
    // the 40px-high box.
    display.setFont(&FreeSerif18pt7b);
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH - 22 - (int)w) / 2 - x1;
    if (x < 2)
        x = 2;
    display.setCursor(x, 44);
    display.print(buf);
    int numRight = x + x1 + (int)w;
    display.setFont(NULL);

    display.setTextSize(1);
    display.setCursor(numRight + 4, 36);
    display.print("Hz");

    if (bioOutputEnabled)
    {
        display.fillRect(2, 54, 30, 10, TFT_WHITE);
        display.setTextColor(TFT_BLACK);
        display.setCursor(6, 56);
        display.print("ON");
        display.setTextColor(TFT_WHITE);
    }
    else
    {
        display.drawRect(2, 54, 30, 10, TFT_WHITE);
        display.setCursor(6, 56);
        display.print("OFF");
    }

    char stepBuf[8];
    snprintf(stepBuf, sizeof(stepBuf), "X%u", bioLastStepSize);
    int16_t  stepX1, stepY1;
    uint16_t stepW, stepH;
    display.getTextBounds(stepBuf, 0, 0, &stepX1, &stepY1, &stepW, &stepH);
    int stepX = SCREEN_WIDTH - 4 - (int)stepW;
    display.setCursor(stepX, 56);
    display.print(stepBuf);

    if (bioOutputEnabled)
    {
        int waveX = 36;
        int waveW = stepX - 4 - waveX;
        bioDrawWaveform(waveX, 54, waveW, 10);
    }

    tftFlush();
}

// Volume screen - reached by short-pressing from Frequency. Same layout
// as the Frequency screen (ON/OFF badge, step badge, waveform), but shows
// the digital volume level (0-100%) and a fill bar instead of Hz.
void bioDrawVolumeMenu()
{
    display.fillScreen(TFT_BLACK);
    display.setTextSize(1);
    display.setTextColor(TFT_WHITE);
    display.setCursor(2, 0);
    display.print("VOLUME");

    // Speaker feedback readout (AMP_LEVEL_ADC), relative/uncalibrated -
    // shown only while output is enabled, since it's meaningless otherwise.
    if (bioOutputEnabled)
    {
        char fbBuf[12];
        snprintf(fbBuf, sizeof(fbBuf), "FB:%u", bioAmpLevel);
        int16_t  fbX1, fbY1;
        uint16_t fbW, fbH;
        display.getTextBounds(fbBuf, 0, 0, &fbX1, &fbY1, &fbW, &fbH);
        display.setCursor(SCREEN_WIDTH - (int)fbW - 2, 0);
        display.print(fbBuf);
    }

    display.drawRect(0, 12, 128, 40, TFT_WHITE);

    char buf[6];
    snprintf(buf, sizeof(buf), "%u", bioVolume);

    display.setFont(&FreeSerif18pt7b);
    int16_t  x1, y1;
    uint16_t w, h;
    display.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
    int x = (SCREEN_WIDTH - 22 - (int)w) / 2 - x1;
    if (x < 2)
        x = 2;
    display.setCursor(x, 40);
    display.print(buf);
    int numRight = x + x1 + (int)w;
    display.setFont(NULL);

    display.setTextSize(1);
    display.setCursor(numRight + 4, 32);
    display.print("%");

    // Level bar across the bottom of the box.
    const int barX = 4, barY = 45, barW = 120, barH = 4;
    display.drawRect(barX, barY, barW, barH, TFT_WHITE);
    int fillW = ((barW - 2) * bioVolume) / BIO_VOLUME_MAX;
    if (fillW > 0)
        display.fillRect(barX + 1, barY + 1, fillW, barH - 2, TFT_WHITE);

    if (bioOutputEnabled)
    {
        display.fillRect(2, 54, 30, 10, TFT_WHITE);
        display.setTextColor(TFT_BLACK);
        display.setCursor(6, 56);
        display.print("ON");
        display.setTextColor(TFT_WHITE);
    }
    else
    {
        display.drawRect(2, 54, 30, 10, TFT_WHITE);
        display.setCursor(6, 56);
        display.print("OFF");
    }

    char stepBuf[8];
    snprintf(stepBuf, sizeof(stepBuf), "X%u", BIO_VOLUME_STEP);
    int16_t  stepX1, stepY1;
    uint16_t stepW, stepH;
    display.getTextBounds(stepBuf, 0, 0, &stepX1, &stepY1, &stepW, &stepH);
    int stepX = SCREEN_WIDTH - 4 - (int)stepW;
    display.setCursor(stepX, 56);
    display.print(stepBuf);

    if (bioOutputEnabled)
    {
        int waveX = 36;
        int waveW = stepX - 4 - waveX;
        bioDrawWaveform(waveX, 54, waveW, 10);
    }

    tftFlush();
}

void bioEnter()
{
    bioMode          = BIO_MAIN;
    // The press that selected "BioResonance Pro" from the main menu is
    // still held; its release edge is still pending. Mark it as already
    // handled so bioHandleButtons() doesn't treat that release as a short
    // press and immediately jump from BIO_MAIN to BIO_PRESET.
    bioButtonWasHeld = true;
    bioButtonDownMs  = millis();
    bioActivateSelectedPreset();
    bioSetOutputEnabled(false);
    bioDrawMainMenu();
}

// Called when encDelta != 0 and appState == STATE_BIORESONANCE.
void bioHandleEncoderDelta(int8_t delta)
{
    if (delta == 0)
        return;

    if (bioMode == BIO_MAIN)
    {
        bioSelectedCategory
            = bioWrappedIndex(bioSelectedCategory + delta, BIO_CATEGORY_COUNT);
        bioDrawMainMenu();
        return;
    }

    if (bioMode == BIO_PRESET)
    {
        uint8_t count = bioCategories[bioSelectedCategory].count;
        bioSelectedPreset[bioSelectedCategory] = bioWrappedIndex(
            bioSelectedPreset[bioSelectedCategory] + delta, count);
        bioActivateSelectedPreset();

        const BioCategory &cat = bioCategories[bioSelectedCategory];
        const BioPreset   &preset
            = cat.items[bioSelectedPreset[bioSelectedCategory]];
        char line[64];
        snprintf(line, sizeof(line), "[%lus] bio: select %s/%s %uHz",
                 millis() / 1000, cat.name, preset.name, preset.hz);
        appendLog(USAGE_FILE, line);

        bioDrawPresetMenu();
        return;
    }

    if (bioMode == BIO_VOLUME)
    {
        int16_t newVolume = (int16_t)bioVolume + delta * (int16_t)BIO_VOLUME_STEP;
        if (newVolume < BIO_VOLUME_MIN)
            newVolume = BIO_VOLUME_MIN;
        if (newVolume > BIO_VOLUME_MAX)
            newVolume = BIO_VOLUME_MAX;
        bioVolume = (uint8_t)newVolume;
        bioApplyVolume();
        bioDrawVolumeMenu();
        return;
    }

    // BIO_FREQUENCY
    uint8_t step     = bioFrequencyStep();
    bioLastStepSize  = step;
    bioFrequencyHz   = bioClampFrequency((int32_t)bioFrequencyHz + delta * step);
    ad9833SetFrequency(bioFrequencyHz);
    bioDrawFrequencyMenu();
}

// Called every loop() iteration while appState == STATE_BIORESONANCE.
// Short press: cycle Main -> Preset -> Main, or Frequency -> Volume ->
// Preset.
// Long press (>=850ms): enter Frequency mode from any screen.
// OUTPUT_SW toggles output in Preset/Frequency/Volume modes. Once enabled,
// output stays on across those screens (and preset/volume changes) until
// OUTPUT_SW is pressed again or the user backs out to the Main Menu.
// Once BioResonance Pro is selected, the device stays in this mode until
// reset - there is no path back to the benchmark menu.
void bioHandleButtons()
{
    if (encSwPressEdge)
    {
        bioButtonDownMs  = millis();
        bioButtonWasHeld = false;
    }

    if (encSwHeld && !bioButtonWasHeld && millis() - bioButtonDownMs >= 850)
    {
        bioButtonWasHeld = true;

        bioMode = BIO_FREQUENCY;
        bioActivateSelectedPreset();
        bioLastFreqTurnMs = millis();
        bioDrawFrequencyMenu();
    }

    if (encSwReleaseEdge && !bioButtonWasHeld)
    {
        if (bioMode == BIO_MAIN)
        {
            bioMode = BIO_PRESET;
            bioActivateSelectedPreset();
            bioDrawPresetMenu();
        }
        else if (bioMode == BIO_PRESET)
        {
            bioMode = BIO_VOLUME;
            bioDrawVolumeMenu();
        }
        else if (bioMode == BIO_FREQUENCY)
        {
            bioMode = BIO_VOLUME;
            bioDrawVolumeMenu();
        }
        else // BIO_VOLUME
        {
            bioMode = BIO_MAIN;
            bioSetOutputEnabled(false);
            bioDrawMainMenu();
        }
    }

    if (encSwReleaseEdge)
        bioButtonWasHeld = false;

    // Front-panel output toggle (debounced).
    static bool     lastOutputBtn = true;
    static uint32_t lastOutputMs  = 0;
    bool outputBtn = digitalRead(BIO_OUTPUT_SW);
    if (lastOutputBtn && !outputBtn && millis() - lastOutputMs > 250
        && bioMode != BIO_MAIN)
    {
        bioSetOutputEnabled(!bioOutputEnabled);
        lastOutputMs = millis();
        if (bioMode == BIO_FREQUENCY)
            bioDrawFrequencyMenu();
        else if (bioMode == BIO_VOLUME)
            bioDrawVolumeMenu();
        else
            bioDrawPresetMenu();
    }
    lastOutputBtn = outputBtn;
}

// Called every loop() iteration while appState == STATE_BIORESONANCE.
// Animates the waveform on the Frequency/Preset/Volume screens while
// output is enabled.
void bioLoop()
{
    if (!bioOutputEnabled || bioMode == BIO_MAIN)
        return;

    static uint32_t lastWaveMs = 0;
    uint32_t        now        = millis();
    if (now - lastWaveMs < 80)
        return;
    lastWaveMs = now;

    bioWavePhase = (bioWavePhase + 15) % 360;
    bioReadAmpLevel();
    if (bioMode == BIO_FREQUENCY)
        bioDrawFrequencyMenu();
    else if (bioMode == BIO_VOLUME)
        bioDrawVolumeMenu();
    else
        bioDrawPresetMenu();
}

void startAppFromMenuIndex(int index)
{
    menuIndex = index;
    appState  = (AppState)stateForMenuIndex(index);
    serviceMenuVisible = (appState == STATE_MENU);

    switch (appState)
    {
        case STATE_PRIMES:
            primesEnter();
            break;
        case STATE_CPU:
            cpuEnter();
            break;
        case STATE_BIORESONANCE:
            bioEnter();
            break;
        case STATE_FILES:
            filesEnter();
            break;
        case STATE_USB_DRIVE:
            usbDriveEnter();
            break;
        case STATE_MENU:
            drawMenu();
            break;
    }
}

void openServiceMenu()
{
    if (appState == STATE_USB_DRIVE)
        usbDriveExit();
    if (appState == STATE_BIORESONANCE)
        bioSetOutputEnabled(false);

    appState = STATE_MENU;
    serviceMenuVisible = true;
    menuIndex = startupMenuIndex;
    drawMenu();
}

bool handleServiceMenuCombo()
{
    bool encRawPressed = (digitalRead(ENC_SW) == LOW);
    bool outputRawPressed = (digitalRead(BIO_OUTPUT_SW) == LOW);
    bool bothHeld = encRawPressed && outputRawPressed;

    if (!bothHeld)
    {
        serviceMenuHoldStartMs = 0;
        serviceMenuComboWasHeld = false;
        return false;
    }

    if (serviceMenuHoldStartMs == 0)
        serviceMenuHoldStartMs = millis();

    if (!serviceMenuComboWasHeld && millis() - serviceMenuHoldStartMs >= 2000)
    {
        serviceMenuComboWasHeld = true;
        openServiceMenu();
    }

    return true;
}

void chooseServiceMenuItem()
{
    startupMenuIndex = menuIndex;
    saveStartupApp();
    serviceMenuVisible = false;
    startAppFromMenuIndex(menuIndex);
}

// ---------------------------------------------------------------------
// Setup / main loop
// ---------------------------------------------------------------------
void setup()
{
    Serial.begin(115200);

    pinMode(ENC_A, INPUT_PULLUP);
    pinMode(ENC_B, INPUT_PULLUP);
    pinMode(ENC_SW, INPUT_PULLUP);
    pinMode(BIO_OUTPUT_SW, INPUT_PULLUP);
    pinMode(BIO_OUTPUT_ENABLE, OUTPUT);
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
    digitalWrite(BIO_OUTPUT_ENABLE, LOW);
    ad9833Init();

    encPrevState = (digitalRead(ENC_A) << 1) | digitalRead(ENC_B);
    attachInterrupt(digitalPinToInterrupt(ENC_A), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(ENC_B), encoderISR, CHANGE);

    display.init(240, 320);
    display.setRotation(1);
    display.fillScreen(TFT_BLACK);
    Serial.println("ST7789 TFT init complete");

    bioApplyVolume(); // set the MCP4551 digital pot to the default level

    // Mount the onboard QSPI flash filesystem (used by "Read Files" and
    // "USB Drive Mode"). If it isn't formatted yet, fsReady stays false
    // and Read Files just shows a message instead of crashing.
    flash.begin();

    // USB Mass Storage - hidden (not ready) until "USB Drive Mode" is
    // selected from the menu, so the host doesn't see the drive while
    // the firmware is using the flash filesystem.
    usb_msc.setID("BioResonance", "Pro Logs", "1.0");
    usb_msc.setReadWriteCallback(mscReadCallback, mscWriteCallback, mscFlushCallback);
    usb_msc.setCapacity(flash.size() / 512, 512);
    usb_msc.setUnitReady(false);
    usb_msc.begin();

    // The MSC interface above must be present in the USB descriptors from
    // first enumeration - if the host already enumerated us (e.g. as just
    // a CDC serial port), force a re-enumeration so it sees MSC too.
    if (TinyUSBDevice.mounted())
    {
        TinyUSBDevice.detach();
        delay(10);
        TinyUSBDevice.attach();
    }

    if (fatfs.begin(&flash))
    {
        fsReady = true;

        // One-time cleanup: remove leftover CircuitPython files from when
        // this board originally shipped with CircuitPython on the flash.
        fatfs.remove("/code.py");
        fatfs.remove("/boot_out.txt");
    }
    else
    {
        Serial.println("Flash filesystem not found/formatted");
    }

    loadStartupApp();
    startAppFromMenuIndex(startupMenuIndex);
}

void loop()
{
    updateEncSw();
    if (handleServiceMenuCombo())
        return;

    // Encoder rotation
    if (encDelta != 0)
    {
        noInterrupts();
        int8_t delta = encDelta;
        encDelta     = 0;
        interrupts();

        if (appState == STATE_MENU)
        {
            menuIndex = (menuIndex + delta + NUM_ITEMS) % NUM_ITEMS;
            if (serviceMenuVisible)
                drawMenu();
        }
        else if (appState == STATE_BIORESONANCE)
        {
            bioHandleEncoderDelta(delta);
        }
        else if (appState == STATE_FILES)
        {
            filesScroll(delta);
        }
    }

    // Button press / release
    if (appState == STATE_BIORESONANCE)
    {
        bioHandleButtons();
    }
    else if (encSwPressEdge)
    {
        if (appState == STATE_MENU)
        {
            if (serviceMenuVisible)
                chooseServiceMenuItem();
        }
        else
        {
            if (appState == STATE_USB_DRIVE)
                usbDriveExit();

            AppState startupState = (AppState)stateForMenuIndex(startupMenuIndex);
            if (appState == startupState)
                openServiceMenu();
            else
                startAppFromMenuIndex(startupMenuIndex);
        }
    }

    switch (appState)
    {
        case STATE_MENU:
            // nothing to do, redraw only on input
            break;
        case STATE_PRIMES:
            primesLoop();
            break;
        case STATE_CPU:
            cpuLoop();
            break;
        case STATE_FILES:
            filesLoop();
            break;
        case STATE_BIORESONANCE:
            bioLoop();
            break;
        case STATE_USB_DRIVE:
            usbDriveLoop();
            break;
    }
}
