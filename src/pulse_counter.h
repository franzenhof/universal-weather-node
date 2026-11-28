#pragma once
#include <stdint.h>

// Hardware pulse counters (ESP-IDF 5 pulse_cnt driver), one per input:
// wind speed, rain, AUX pulse (V4 Full) and the fan tacho.
// The 16-bit hardware counter overflows at PULSE_COUNTER_HIGH_LIMIT; the
// driver accumulates the overflows (accum_count), so a read returns all
// pulses since the last start, as 32 bits that wrap around at 2^32 — take
// differences with unsigned arithmetic.
enum PulseCounterId : uint8_t {
    PCNT_UNIT_WINDSPEED = 0,
    PCNT_UNIT_RAIN      = 1,
    PCNT_UNIT_AUX_PULSE = 2,
    PCNT_UNIT_TACHO     = 3,
    PULSE_COUNTER_COUNT = 4,
};

constexpr int      PULSE_COUNTER_HIGH_LIMIT = 32767;
// Pulses shorter than this are ignored; the RC filter on the board does the rest.
constexpr uint32_t PULSE_COUNTER_GLITCH_NS  = 1250;

// The first call creates the counter on `gpio` (falling or rising edge);
// every call clears it and starts counting from 0. GPIO and edge of an
// existing counter are kept.
bool     pulseCounterStart(PulseCounterId id, int gpio, bool countFalling);
// Stops and clears the counter (no-op if it was never started).
void     pulseCounterStop(PulseCounterId id);
// Pulses since the last start.
uint32_t pulseCounterRead(PulseCounterId id);

// The driver adds an overflow when its interrupt runs; a read between the
// hardware wrap and that interrupt comes back PULSE_COUNTER_HIGH_LIMIT too
// low. The counters only count up, so a value below the previous one means
// exactly that: add the pending overflow (the next read after the interrupt
// is right again). Valid while fewer than 32767 pulses pass between reads.
inline uint32_t pulseCounterMonotonic(uint32_t raw, uint32_t prev)
{
    return (int32_t)(raw - prev) < 0 ? raw + (uint32_t)PULSE_COUNTER_HIGH_LIMIT : raw;
}
