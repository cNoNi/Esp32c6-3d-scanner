#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "camera.hpp"
#include "motor.hpp"

struct ScannerConfig {
    int photos_per_revolution;
    int steps_per_revolution;
    int resolution;
    uint32_t pre_wait_ms;
    uint32_t post_wait_ms;
};

struct ScanStatus {
    bool running;
    int photo;
    int total;
    bool has_error;
    std::string error;
};

// Turntable scan sequence: move -> wait -> photo -> wait -> next position.
// The scan runs in its own FreeRTOS task.
class Scanner {
public:
    using StatusCallback = std::function<void(const ScanStatus&)>;

    Scanner(Motor& motor, Camera& camera, const ScannerConfig& config);

    esp_err_t init();

    void set_status_callback(StatusCallback callback) { on_status_change_ = std::move(callback); }

    // Returns false if a scan is already running
    bool start();
    void stop();

    bool running() const { return running_; }
    ScanStatus status();

private:
    static void task_entry(void* arg);
    void scan();
    void motor_setup();
    bool wait_ms(uint32_t ms);  // returns false if the scan was stopped
    void notify();

    Motor& motor_;
    Camera& camera_;
    ScannerConfig config_;
    int microsteps_per_revolution_;

    std::atomic<bool> running_{false};

    std::mutex status_mutex_;
    int current_photo_ = 0;
    bool has_error_ = false;
    std::string error_;

    StatusCallback on_status_change_;

    TaskHandle_t task_ = nullptr;
    SemaphoreHandle_t start_signal_ = nullptr;
    SemaphoreHandle_t stop_signal_ = nullptr;
};
