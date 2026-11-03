#pragma once
#include <stdint.h>
using pcnt_unit_t = int;
constexpr int PCNT_UNIT_0=0, PCNT_UNIT_1=1, PCNT_UNIT_2=2, PCNT_UNIT_3=3;
constexpr int PCNT_CHANNEL_0=0, PCNT_PIN_NOT_USED=-1, PCNT_COUNT_DIS=0, PCNT_COUNT_INC=1;
struct pcnt_config_t {
    int pulse_gpio_num,ctrl_gpio_num,pos_mode,neg_mode,counter_h_lim,counter_l_lim,unit,channel;
};
extern int16_t hostCounts[4];
inline void pcnt_unit_config(pcnt_config_t*) {}
inline void pcnt_set_filter_value(int,int) {}
inline void pcnt_filter_enable(int) {}
inline void pcnt_counter_pause(int) {}
inline void pcnt_counter_clear(int unit) { hostCounts[unit]=0; }
inline void pcnt_counter_resume(int) {}
inline void pcnt_get_counter_value(int unit,int16_t* out) { *out=hostCounts[unit]; }
