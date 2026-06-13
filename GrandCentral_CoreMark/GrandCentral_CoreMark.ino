/*
  CoreMark benchmark for the Adafruit Grand Central M4 (SAMD51).

  This runs the official EEMBC CoreMark 1.0 benchmark (sources downloaded
  unmodified except for the core_portme.* port files and ee_printf.cpp,
  which were adapted to use Arduino's micros() for timing and Serial for
  output).

  For best (and comparable-to-published) results, build with:
    Tools > Optimize: "Faster (-O3)"  [if available for this core]
  Default settings will still work, just with a lower score.

  The run will take roughly 10-20 seconds. Watch the Serial Monitor
  at 115200 baud.
*/

#include "coremark.h"

void coremark_main();

void setup()
{
    Serial.begin(115200);
    while (!Serial)
    {
        delay(10); // wait for native USB serial to connect
    }

    delay(1000);
    Serial.println();
    Serial.println("=== Grand Central M4 (SAMD51 @ 120MHz) CoreMark ===");
    Serial.println("Running... this takes about 10-20 seconds.");
    Serial.println();

    coremark_main();

    Serial.println();
    Serial.println("=== Done ===");
    Serial.println("CoreMark/MHz = score / 120");
}

void loop()
{
    // nothing to do, benchmark already ran in setup()
}
