#pragma once

// 3D Scanner configuration (port of config.py).
// Values come from menuconfig: idf.py menuconfig -> "3D Scanner configuration".

#include <cstdint>

#include "driver/gpio.h"
#include "sdkconfig.h"

namespace cfg {

// -------------------------
// Motor configuration
// -------------------------

constexpr gpio_num_t MOTOR_STEP_PIN = static_cast<gpio_num_t>(CONFIG_SCANNER_MOTOR_STEP_GPIO);
constexpr gpio_num_t MOTOR_DIR_PIN = static_cast<gpio_num_t>(CONFIG_SCANNER_MOTOR_DIR_GPIO);
constexpr gpio_num_t MOTOR_ENABLE_PIN = static_cast<gpio_num_t>(CONFIG_SCANNER_MOTOR_ENABLE_GPIO);

constexpr int32_t MOTOR_JOG = CONFIG_SCANNER_MOTOR_JOG;

// 200 steps/revolution motor (1.8 degree per step)
constexpr int MOTOR_STEPS_PER_REVOLUTION = CONFIG_SCANNER_MOTOR_STEPS_PER_REVOLUTION;
// Microstepping resolution, i.e. MOTOR_STEPS_PER_REVOLUTION * MOTOR_RESOLUTION
// = number of microsteps for a full revolution
constexpr int MOTOR_RESOLUTION = CONFIG_SCANNER_MOTOR_RESOLUTION;

// delay between GPIO pulses, lower = faster
constexpr uint32_t MOTOR_STEP_DELAY_US = CONFIG_SCANNER_MOTOR_STEP_DELAY_US;

// -------------------------
// Scanner configuration
// -------------------------

constexpr int PHOTOS_PER_REVOLUTION = CONFIG_SCANNER_PHOTOS_PER_REVOLUTION;

// wait after moving for turntable vibrations to stop
constexpr uint32_t PRE_CAPTURE_DELAY_MS = CONFIG_SCANNER_PRE_CAPTURE_DELAY_MS;

// wait after photo before next movement
constexpr uint32_t POST_CAPTURE_DELAY_MS = CONFIG_SCANNER_POST_CAPTURE_DELAY_MS;

// -------------------------
// Camera (phone) configuration
// -------------------------

constexpr uint32_t PHONE_SEND_TIMEOUT_MS = CONFIG_SCANNER_PHONE_SEND_TIMEOUT_MS;
constexpr uint32_t PHOTO_CONFIRM_TIMEOUT_MS = CONFIG_SCANNER_PHOTO_CONFIRM_TIMEOUT_MS;

// -------------------------
// Web server
// -------------------------

constexpr uint16_t SERVER_PORT = CONFIG_SCANNER_SERVER_PORT;
constexpr uint16_t WEBSOCKET_PORT = CONFIG_SCANNER_WEBSOCKET_PORT;

}  // namespace cfg
