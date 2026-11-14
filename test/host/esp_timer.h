#pragma once
#include <Arduino.h>
#include <cassert>
using esp_err_t = int;
constexpr esp_err_t ESP_OK = 0;
#define ESP_ERROR_CHECK(call) assert((call) == ESP_OK)
enum esp_timer_dispatch_t { ESP_TIMER_TASK };
struct esp_timer_create_args_t {
    void (*callback)(void*);
    void* arg;
    esp_timer_dispatch_t dispatch_method;
    const char* name;
    bool skip_unhandled_events;
};
struct HostTimer {
    esp_timer_create_args_t args{};
    bool running = false;
    uint64_t periodUs = 0;
};
using esp_timer_handle_t = HostTimer*;
inline HostTimer& hostHeaterTimer() { static HostTimer timer; return timer; }
inline int64_t esp_timer_get_time() { return (int64_t)millis()*1000; }
inline esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* handle) {
    hostHeaterTimer().args = *args;
    *handle = &hostHeaterTimer();
    return ESP_OK;
}
inline esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period) {
    assert(!timer->running);
    timer->running = true;
    timer->periodUs = period;
    return ESP_OK;
}
inline esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
    assert(timer->running);
    timer->running = false;
    return ESP_OK;
}
inline void hostRunTimers() {
    auto& timer = hostHeaterTimer();
    if (timer.running) timer.args.callback(timer.args.arg);
}
