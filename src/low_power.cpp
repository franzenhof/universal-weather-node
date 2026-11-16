#include "low_power.h"
#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_task_wdt.h>
#include <driver/gpio.h>
#include "config.h"
#include "globals.h"
#include "sensors.h"
#include "board_io.h"
#include "serial_log.h"

static constexpr int PIN_BUTTON = 0;     // PRG button, LOW = pressed

bool lowPowerActive()
{
    // Wireless-sensor reception keeps the SX1262 in receive mode: no light sleep.
    return gCfg.lowPowerEn && gCfg.loraEn && !gCfg.wifiEn && !gCfg.rfEn
        && !gIo.heaterOn && !gIo.fanOn;
}

void lowPowerSleep(unsigned long maxMs)
{
    if (maxMs < 30 || digitalRead(PIN_BUTTON) == LOW) {
        vTaskDelay(pdMS_TO_TICKS(maxMs < 50 ? maxMs : 50));
        return;
    }

    // Rev 1.3+ has an inverting Schmitt trigger in front of the GPIO: a
    // sensor pulse is the falling edge there (Rev 1.2: rising) - the same
    // edge the PCNT units count. The AUX pulse input (v2.0) is always inverted.
    const bool inverted = (gCfg.pcbVersion >= PCB_REV_1_3);
    struct Input { bool en; int pin; volatile uint32_t* count; bool inv; int level; };
    Input in[3] = { { gCfg.windEn != 0,      gPins.windSpeed, &sleepPulsesWind, inverted, 0 },
                    { gCfg.rainEn != 0,      gPins.rain,      &sleepPulsesRain, inverted, 0 },
                    { auxPulseInputUsed(),   PIN_AUX_PULSE,   &sleepPulsesAux,  true,     0 } };

    Serial.flush();
    const int64_t end = esp_timer_get_time() + (int64_t)maxMs * 1000;
    for (;;) {
        int64_t rem = end - esp_timer_get_time();
        if (rem < 20000) break;

        // Level wake-up on the opposite of the current level = wake on every
        // edge of the reed contacts.
        for (Input& i : in) {
            if (!i.en) continue;
            i.level = digitalRead(i.pin);
            gpio_wakeup_enable((gpio_num_t)i.pin, i.level ? GPIO_INTR_LOW_LEVEL : GPIO_INTR_HIGH_LEVEL);
        }
        gpio_wakeup_enable((gpio_num_t)PIN_BUTTON, GPIO_INTR_LOW_LEVEL);
        esp_sleep_enable_gpio_wakeup();
        esp_sleep_enable_timer_wakeup((uint64_t)rem);
        esp_light_sleep_start();

        for (Input& i : in) {
            if (!i.en) continue;
            int now = digitalRead(i.pin);
            if (now != i.level && (i.inv ? now == LOW : now == HIGH)) (*i.count)++;
            gpio_wakeup_disable((gpio_num_t)i.pin);
        }
        gpio_wakeup_disable((gpio_num_t)PIN_BUTTON);
        esp_task_wdt_reset();
        if (digitalRead(PIN_BUTTON) == LOW) break;   // let loop() handle the button
    }
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_GPIO);
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);
}
