#include "motor.hpp"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"

namespace {
const char* TAG = "motor";

// TMC2209 needs DIR stable >20 ns before STEP; 5 us is plenty
constexpr uint32_t DIR_SETUP_US = 5;
constexpr uint32_t TIMER_RESOLUTION_HZ = 1000000;  // 1 tick = 1 us
}  // namespace

Motor::Motor(gpio_num_t en_pin, gpio_num_t step_pin, gpio_num_t dir_pin, uint32_t step_delay_us)
    : en_pin_(en_pin), step_pin_(step_pin), dir_pin_(dir_pin), step_delay_us_(step_delay_us)
{
}

esp_err_t Motor::init()
{
    // Preload output levels so the driver stays disabled when outputs turn on
    gpio_set_level(en_pin_, 1);
    gpio_set_level(step_pin_, 0);
    gpio_set_level(dir_pin_, 0);

    gpio_config_t io = {};
    io.pin_bit_mask = (1ULL << en_pin_) | (1ULL << step_pin_) | (1ULL << dir_pin_);
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "GPIO config failed");

    // start: driver disabled
    set_motor_enabled(false);
    gpio_set_level(step_pin_, 0);

    done_ = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(done_ != nullptr, ESP_ERR_NO_MEM, TAG, "semaphore alloc failed");

    gptimer_config_t timer_cfg = {};
    timer_cfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    timer_cfg.direction = GPTIMER_COUNT_UP;
    timer_cfg.resolution_hz = TIMER_RESOLUTION_HZ;
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer_cfg, &timer_), TAG, "timer create failed");

    gptimer_event_callbacks_t cbs = {};
    cbs.on_alarm = &Motor::on_alarm;
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(timer_, &cbs, this), TAG, "timer callback failed");
    ESP_RETURN_ON_ERROR(gptimer_enable(timer_), TAG, "timer enable failed");

    ESP_LOGI(TAG, "Motor ready: STEP=%d DIR=%d EN=%d, step delay %lu us",
             static_cast<int>(step_pin_), static_cast<int>(dir_pin_), static_cast<int>(en_pin_),
             static_cast<unsigned long>(step_delay_us_));
    return ESP_OK;
}

void Motor::set_motor_enabled(bool enabled)
{
    // EN = LOW -> enabled, EN = HIGH -> disabled
    gpio_set_level(en_pin_, enabled ? 0 : 1);
}

void Motor::run_to_position_steps(int32_t target)
{
    const int32_t current = position_;
    if (target == current) {
        return;
    }

    uint32_t steps;
    if (target > current) {
        gpio_set_level(dir_pin_, 1);
        steps = static_cast<uint32_t>(target - current);
    } else {
        gpio_set_level(dir_pin_, 0);
        steps = static_cast<uint32_t>(current - target);
    }
    esp_rom_delay_us(DIR_SETUP_US);

    pulse(steps);

    position_ = target;
}

void Motor::pulse(uint32_t steps)
{
    // Each step: HIGH for step_delay, then LOW for step_delay -> 2 edges per step
    step_level_ = false;
    gpio_set_level(step_pin_, 0);
    remaining_edges_ = steps * 2;

    gptimer_alarm_config_t alarm = {};
    alarm.alarm_count = step_delay_us_;
    alarm.reload_count = 0;
    alarm.flags.auto_reload_on_alarm = true;
    ESP_ERROR_CHECK(gptimer_set_alarm_action(timer_, &alarm));
    ESP_ERROR_CHECK(gptimer_set_raw_count(timer_, 0));

    xSemaphoreTake(done_, 0);
    ESP_ERROR_CHECK(gptimer_start(timer_));
    xSemaphoreTake(done_, portMAX_DELAY);
}

bool Motor::on_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t*, void* user_ctx)
{
    auto* self = static_cast<Motor*>(user_ctx);

    self->step_level_ = !self->step_level_;
    gpio_set_level(self->step_pin_, self->step_level_ ? 1 : 0);

    const uint32_t remaining = self->remaining_edges_ - 1;
    self->remaining_edges_ = remaining;
    if (remaining == 0) {
        gptimer_stop(timer);
        BaseType_t woken = pdFALSE;
        xSemaphoreGiveFromISR(self->done_, &woken);
        return woken == pdTRUE;
    }
    return false;
}

void Motor::close()
{
    set_motor_enabled(false);
    if (timer_) {
        gptimer_disable(timer_);
        gptimer_del_timer(timer_);
        timer_ = nullptr;
    }
    gpio_reset_pin(step_pin_);
    gpio_reset_pin(dir_pin_);
    // EN is left as output HIGH so the driver stays disabled
}
