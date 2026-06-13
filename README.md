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

## License

CoreMark core sources are Copyright EEMBC, licensed under the
[Apache License 2.0](https://www.apache.org/licenses/LICENSE-2.0).
