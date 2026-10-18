#include "scanner.hpp"

#include <cmath>
#include <cstdio>

#include "esp_check.h"
#include "esp_log.h"

namespace {
const char* TAG = "scanner";
constexpr uint32_t TASK_STACK = 4096;
constexpr UBaseType_t TASK_PRIORITY = 5;
}  // namespace

Scanner::Scanner(Motor& motor, Camera& camera, const ScannerConfig& config)
    : motor_(motor),
      camera_(camera),
      config_(config),
      microsteps_per_revolution_(config.steps_per_revolution * config.resolution)
{
}

esp_err_t Scanner::init()
{
    start_signal_ = xSemaphoreCreateBinary();
    stop_signal_ = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(start_signal_ && stop_signal_, ESP_ERR_NO_MEM, TAG, "alloc failed");

    BaseType_t ok = xTaskCreate(&Scanner::task_entry, "scanner", TASK_STACK, this, TASK_PRIORITY, &task_);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "task create failed");
    return ESP_OK;
}

void Scanner::notify()
{
    if (on_status_change_) {
        on_status_change_(status());
    }
}

void Scanner::motor_setup()
{
    motor_.set_motor_enabled(true);
    motor_.run_to_position_steps(0);
    motor_.set_microstepping_resolution(config_.resolution);
}

bool Scanner::start()
{
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) {
        return false;  // scan already running
    }

    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        current_photo_ = 0;
        has_error_ = false;
        error_.clear();
    }
    notify();

    xSemaphoreGive(start_signal_);
    return true;
}

void Scanner::stop()
{
    running_ = false;
    xSemaphoreGive(stop_signal_);  // interrupt current wait
}

ScanStatus Scanner::status()
{
    std::lock_guard<std::mutex> lock(status_mutex_);
    return ScanStatus{running_, current_photo_, config_.photos_per_revolution, has_error_, error_};
}

bool Scanner::wait_ms(uint32_t ms)
{
    if (ms > 0) {
        xSemaphoreTake(stop_signal_, pdMS_TO_TICKS(ms));
    }
    return running_;
}

void Scanner::task_entry(void* arg)
{
    auto* self = static_cast<Scanner*>(arg);
    for (;;) {
        xSemaphoreTake(self->start_signal_, portMAX_DELAY);
        self->scan();
    }
}

void Scanner::scan()
{
    xSemaphoreTake(stop_signal_, 0);  // drop stale stop signal

    std::lock_guard<std::mutex> motor_lock(motor_.mutex());

    ESP_LOGI(TAG, "Scan started: %d photos", config_.photos_per_revolution);

    motor_setup();

    const double step_per_photo =
        static_cast<double>(microsteps_per_revolution_) / config_.photos_per_revolution;

    for (int photo = 0; photo < config_.photos_per_revolution; ++photo) {
        if (!running_) {
            break;
        }

        if (!wait_ms(config_.pre_wait_ms)) {
            break;
        }

        char filename[24];
        snprintf(filename, sizeof(filename), "image_%04d.jpg", photo);

        std::string error;
        if (!camera_.take_photo(filename, &error)) {
            ESP_LOGE(TAG, "Scan aborted: %s", error.c_str());
            std::lock_guard<std::mutex> lock(status_mutex_);
            has_error_ = true;
            error_ = error;
            break;
        }

        wait_ms(config_.post_wait_ms);

        {
            std::lock_guard<std::mutex> lock(status_mutex_);
            current_photo_ = photo + 1;
        }
        notify();

        if (photo == config_.photos_per_revolution - 1) {
            break;
        }

        // nearbyint rounds half to even, same as Python round()
        motor_.run_to_position_steps(static_cast<int32_t>(std::nearbyint((photo + 1) * step_per_photo)));
    }

    running_ = false;
    motor_.set_motor_enabled(false);
    notify();

    ESP_LOGI(TAG, "Scan finished");
}
