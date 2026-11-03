#pragma once

// Low-power mode (lowPowerEn, LoRa-only operation): instead of idling in
// loop(), the chip goes into light sleep until the next sub-sample. RAM,
// the LoRaWAN session and the chart history stay intact. Wind-speed and
// rain pulses wake it up and are counted in software while it sleeps
// (the PCNT units are not clocked in light sleep).

// True while low-power operation is allowed right now: enabled, LoRa on,
// Wi-Fi off, heater/fan not running (their PWM stops in light sleep).
// Wind, rain and AUX pulses are counted in software while asleep.
bool lowPowerActive();

// Sleeps for up to maxMs, waking for every wind/rain/AUX edge (counted into
// sleepPulsesWind/Rain/Aux) and for the user button. Returns early on a button
// press; shorter waits than ~30 ms just yield.
void lowPowerSleep(unsigned long maxMs);
