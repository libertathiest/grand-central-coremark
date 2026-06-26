/*
  BioResonancePro SAMD51 signal-chain bench test.

  Purpose:
    - Verify shared I2C bus: OLED at 0x3C and MCP4551 at 0x2F.
    - Verify MCP4551 digital volume writes.
    - Verify AD9833 SPI frequency output.
    - Bring amplifier enable up safely at low volume.

  Serial Monitor: 115200 baud, newline optional.

  Commands:
    ?       help
    s       scan I2C bus
    o       toggle output enable
    on      output enable on
    off     output enable off
    vNN     set volume percent, example v10
    fNN     set frequency Hz, example f100
    r       slow volume ramp at current frequency
    t       tone sequence: 30, 100, 300, 1000, 2000 Hz
*/

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

// Same pin assumptions as the main Grand Central program.
#define BIO_OUTPUT_ENABLE 6
#define AD9833_FSYNC 10

#define MCP4551_I2C_ADDR  0x2F
#define MCP4551_WIPER_CMD 0x00

const uint32_t AD9833_MCLK_HZ = 25000000UL;
const uint16_t AD9833_B28 = 0x2000;
const uint16_t AD9833_RESET = 0x0100;
const uint16_t AD9833_SLEEP1 = 0x0080;
const uint16_t AD9833_SLEEP12 = 0x0040;
const uint16_t AD9833_FREQ0_WRITE = 0x4000;
const SPISettings AD9833_SPI_SETTINGS(2000000, MSBFIRST, SPI_MODE2);

bool outputEnabled = false;
uint16_t frequencyHz = 100;
uint8_t volumePercent = 0;

void ad9833WriteRegister(uint16_t data)
{
  digitalWrite(AD9833_FSYNC, LOW);
  SPI.transfer16(data);
  digitalWrite(AD9833_FSYNC, HIGH);
}

void ad9833SetSleep(bool sleep)
{
  SPI.beginTransaction(AD9833_SPI_SETTINGS);
  ad9833WriteRegister(AD9833_B28 | (sleep ? (AD9833_SLEEP1 | AD9833_SLEEP12) : 0));
  SPI.endTransaction();
}

void ad9833SetFrequency(uint32_t freqHz)
{
  uint32_t freqWord = (uint32_t)(((uint64_t)freqHz << 28) / AD9833_MCLK_HZ);
  uint16_t lsb = (uint16_t)(freqWord & 0x3FFF) | AD9833_FREQ0_WRITE;
  uint16_t msb = (uint16_t)((freqWord >> 14) & 0x3FFF) | AD9833_FREQ0_WRITE;

  SPI.beginTransaction(AD9833_SPI_SETTINGS);
  ad9833WriteRegister(AD9833_B28 | AD9833_RESET);
  ad9833WriteRegister(lsb);
  ad9833WriteRegister(msb);
  ad9833WriteRegister(AD9833_B28);
  SPI.endTransaction();
}

void ad9833Init()
{
  pinMode(AD9833_FSYNC, OUTPUT);
  digitalWrite(AD9833_FSYNC, HIGH);
  SPI.begin();
  ad9833SetFrequency(frequencyHz);
  ad9833SetSleep(true);
}

bool setVolumePercent(uint8_t percent)
{
  if (percent > 100)
    percent = 100;

  volumePercent = percent;
  uint8_t wiper = map(volumePercent, 0, 100, 0, 127);

  Wire.beginTransmission(MCP4551_I2C_ADDR);
  Wire.write(MCP4551_WIPER_CMD << 4);
  Wire.write(wiper);
  uint8_t err = Wire.endTransmission();

  Serial.print("Volume ");
  Serial.print(volumePercent);
  Serial.print("%, wiper ");
  Serial.print(wiper);
  Serial.print(", I2C ");
  Serial.println(err == 0 ? "OK" : "ERROR");

  return err == 0;
}

void setOutputEnabled(bool enabled)
{
  if (enabled)
    setVolumePercent(min<uint8_t>(volumePercent, 5));

  outputEnabled = enabled;
  digitalWrite(BIO_OUTPUT_ENABLE, outputEnabled ? HIGH : LOW);
  ad9833SetSleep(!outputEnabled);

  Serial.print("Output ");
  Serial.println(outputEnabled ? "ON" : "OFF");
}

void scanI2c()
{
  Serial.println("I2C scan:");
  for (uint8_t addr = 1; addr < 127; addr++)
  {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0)
    {
      Serial.print("  found 0x");
      if (addr < 16)
        Serial.print('0');
      Serial.println(addr, HEX);
    }
  }
  Serial.println("Expected: OLED 0x3C, MCP4551 0x2F");
}

void printHelp()
{
  Serial.println();
  Serial.println("SignalChain_Test commands:");
  Serial.println("  ?     help");
  Serial.println("  s     scan I2C bus");
  Serial.println("  o     toggle output enable");
  Serial.println("  on    output enable on");
  Serial.println("  off   output enable off");
  Serial.println("  vNN   set volume percent, example v10");
  Serial.println("  fNN   set frequency Hz, example f100");
  Serial.println("  r     ramp volume 0..30%");
  Serial.println("  t     tone sequence 30..2000 Hz");
  Serial.println();
}

void runVolumeRamp()
{
  Serial.println("Volume ramp 0..30%. Output will be enabled.");
  setVolumePercent(0);
  setOutputEnabled(true);
  for (uint8_t v = 0; v <= 30; v += 2)
  {
    setVolumePercent(v);
    delay(500);
  }
  Serial.println("Ramp complete.");
}

void runToneSequence()
{
  const uint16_t tones[] = { 30, 100, 300, 1000, 2000 };
  Serial.println("Tone sequence. Output will be enabled at current volume.");
  setOutputEnabled(true);
  for (uint8_t i = 0; i < sizeof(tones) / sizeof(tones[0]); i++)
  {
    frequencyHz = tones[i];
    ad9833SetFrequency(frequencyHz);
    Serial.print("Frequency ");
    Serial.print(frequencyHz);
    Serial.println(" Hz");
    delay(2500);
  }
  Serial.println("Tone sequence complete.");
}

void handleCommand(String cmd)
{
  cmd.trim();
  cmd.toLowerCase();
  if (cmd.length() == 0)
    return;

  if (cmd == "?")
    printHelp();
  else if (cmd == "s")
    scanI2c();
  else if (cmd == "o")
    setOutputEnabled(!outputEnabled);
  else if (cmd == "on")
    setOutputEnabled(true);
  else if (cmd == "off")
    setOutputEnabled(false);
  else if (cmd[0] == 'v')
    setVolumePercent((uint8_t)constrain(cmd.substring(1).toInt(), 0, 100));
  else if (cmd[0] == 'f')
  {
    frequencyHz = (uint16_t)constrain(cmd.substring(1).toInt(), 30, 2000);
    ad9833SetFrequency(frequencyHz);
    Serial.print("Frequency ");
    Serial.print(frequencyHz);
    Serial.println(" Hz");
  }
  else if (cmd == "r")
    runVolumeRamp();
  else if (cmd == "t")
    runToneSequence();
  else
    Serial.println("Unknown command. Type ? for help.");
}

void setup()
{
  pinMode(BIO_OUTPUT_ENABLE, OUTPUT);
  digitalWrite(BIO_OUTPUT_ENABLE, LOW);

  Serial.begin(115200);
  delay(1500);

  Wire.begin();
  Wire.setClock(400000);
  ad9833Init();
  setVolumePercent(0);
  setOutputEnabled(false);

  Serial.println("BioResonancePro signal-chain bench test");
  printHelp();
  scanI2c();
}

void loop()
{
  if (Serial.available())
    handleCommand(Serial.readStringUntil('\n'));
}
