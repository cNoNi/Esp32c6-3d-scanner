#include "motor_tests.hpp"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {
const char* TAG = "motor_test";
}  // namespace

void run_motor_test(Motor& motor)
{
    std::lock_guard<std::mutex> lock(motor.mutex());

    // 400 steps/s -> half period = 1 / (400 * 2) s = 1250 us
    constexpr uint32_t steps_per_second = 400;
    motor.set_step_delay_us(1000000 / (steps_per_second * 2));
    motor.set_motor_enabled(true);

    ESP_LOGI(TAG, "Spinning forward at %lu steps/s", static_cast<unsigned long>(steps_per_second));

    for (;;) {
        // one second of motion per call; reset so the position never overflows
        motor.reset_position(0);
        motor.run_to_position_steps(static_cast<int32_t>(steps_per_second));
    }
}

void run_move_test(Motor& motor)
{
    std::lock_guard<std::mutex> lock(motor.mutex());

    ESP_LOGI(TAG, "Move test: 0 -> 200 -> 0");

    motor.set_motor_enabled(true);
    motor.run_to_position_steps(200);
    vTaskDelay(pdMS_TO_TICKS(1000));
    motor.run_to_position_steps(0);
    motor.set_motor_enabled(false);

    ESP_LOGI(TAG, "Move test done");
}
