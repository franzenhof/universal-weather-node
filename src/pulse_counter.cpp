#include "pulse_counter.h"
#include "serial_log.h"

#include <driver/pulse_cnt.h>

namespace {

struct Counter {
    pcnt_unit_handle_t unit = nullptr;
    bool     running = false;
    uint32_t last    = 0;   // last value returned (pulseCounterMonotonic)
};
Counter counters[PULSE_COUNTER_COUNT];

bool create(Counter& c, int gpio, bool countFalling)
{
    pcnt_unit_config_t uc = {};
    uc.low_limit  = -1;   // only counts up; the driver wants a limit below 0
    uc.high_limit = PULSE_COUNTER_HIGH_LIMIT;
    uc.flags.accum_count = 1;
    pcnt_unit_handle_t unit = nullptr;
    if (pcnt_new_unit(&uc, &unit) != ESP_OK) return false;

    pcnt_glitch_filter_config_t fc = { .max_glitch_ns = PULSE_COUNTER_GLITCH_NS };
    pcnt_chan_config_t cc = {};
    cc.edge_gpio_num  = gpio;
    cc.level_gpio_num = -1;
    pcnt_channel_handle_t ch = nullptr;
    const pcnt_channel_edge_action_t inc  = PCNT_CHANNEL_EDGE_ACTION_INCREASE;
    const pcnt_channel_edge_action_t hold = PCNT_CHANNEL_EDGE_ACTION_HOLD;
    if (pcnt_unit_set_glitch_filter(unit, &fc) != ESP_OK
        || pcnt_new_channel(unit, &cc, &ch) != ESP_OK
        // (rising edge action, falling edge action)
        || pcnt_channel_set_edge_action(ch, countFalling ? hold : inc, countFalling ? inc : hold) != ESP_OK
        || pcnt_channel_set_level_action(ch, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP) != ESP_OK
        // accum_count needs a watch point at the limit to see the overflow
        || pcnt_unit_add_watch_point(unit, PULSE_COUNTER_HIGH_LIMIT) != ESP_OK
        || pcnt_unit_enable(unit) != ESP_OK) {
        if (ch) pcnt_del_channel(ch);
        pcnt_del_unit(unit);
        return false;
    }
    c.unit = unit;
    return true;
}

} // namespace

bool pulseCounterStart(PulseCounterId id, int gpio, bool countFalling)
{
    if (id >= PULSE_COUNTER_COUNT) return false;
    Counter& c = counters[id];
    if (!c.unit && !create(c, gpio, countFalling)) {
        dbgSerial.printf("[PCNT] counter %u on GPIO%d: setup failed\n", (unsigned)id, gpio);
        return false;
    }
    if (c.running) pcnt_unit_stop(c.unit);
    pcnt_unit_clear_count(c.unit);
    c.last = 0;
    c.running = pcnt_unit_start(c.unit) == ESP_OK;
    return c.running;
}

void pulseCounterStop(PulseCounterId id)
{
    if (id >= PULSE_COUNTER_COUNT || !counters[id].unit) return;
    Counter& c = counters[id];
    if (c.running) pcnt_unit_stop(c.unit);
    c.running = false;
    pcnt_unit_clear_count(c.unit);
    c.last = 0;
}

uint32_t pulseCounterRead(PulseCounterId id)
{
    if (id >= PULSE_COUNTER_COUNT || !counters[id].unit) return 0;
    Counter& c = counters[id];
    int v = 0;
    if (pcnt_unit_get_count(c.unit, &v) != ESP_OK) return c.last;
    c.last = pulseCounterMonotonic((uint32_t)v, c.last);
    return c.last;
}
