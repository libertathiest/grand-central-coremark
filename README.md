# Grand Central M4 CoreMark

An Arduino port of the [EEMBC CoreMark 1.0](https://github.com/eembc/coremark)
benchmark for the [Adafruit Grand Central M4](https://www.adafruit.com/product/4064)
(SAMD51, Cortex-M4F @ 120MHz).

The core benchmark algorithms (`core_list_join`, `core_matrix`, `core_state`,
`core_util`, `core_main`, `coremark.h`) are the unmodified EEMBC sources.
The `core_portme.*` port files and `ee_printf.cpp` have been adapted to:

- Use `micros()` for timing
- Output results over USB `Serial` (115200 baud)
- Auto-calibrate iteration count to run for ~10-20 seconds
- Build cleanly under C++ with the Adafruit SAMD board package (Arduino IDE)
- Provide `ecvtbuf`/`fcvtbuf` implementations missing from this toolchain's newlib

## Usage

1. Open `GrandCentral_CoreMark/GrandCentral_CoreMark.ino` in the Arduino IDE.
2. Install the **Adafruit SAMD Boards** package (Boards Manager) and select
   **Adafruit Grand Central M4**.
3. Upload, then open the Serial Monitor at **115200 baud**.
4. After ~10-20 seconds it prints the CoreMark score (iterations/sec) and
   validation values. Divide the score by 120 for CoreMark/MHz.

## Arduino IDE Progress Sketch

`GrandCentral_Benchmark_Menu/GrandCentral_Benchmark_Menu.ino` is the current
Grand Central M4 Arduino IDE progress sketch. It combines:

- Rotary encoder + 128x64 OLED menu
- Prime-number and CPU-load benchmark screens
- BioResonance Pro menu prototype
- AD9833 DDS control on SPI with FSYNC on D10
- MCP4551 digital volume control on I2C
- Relative speaker feedback readback on A0
- QSPI flash log browser and optional USB mass-storage log access

Board settings used for the latest local compile:

- Board: **Adafruit Grand Central M4 (SAMD51)**
- USB Stack: **TinyUSB**
- CPU Speed: **120 MHz**
- Optimization: **Small (-Os)**
- QSPI Speed: **50 MHz**

The sketch will not compile with the default Arduino USB stack because it uses
`Adafruit_TinyUSB` for USB drive mode.

Before connecting a speaker, verify the D6 amplifier control polarity on the
bench. The sketch treats D6 as an active-high output enable, while the hardware
net has also been called `AMP_MUTE` in the daughter-board schematic.

## License

CoreMark core sources are Copyright EEMBC, licensed under the
[Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).
