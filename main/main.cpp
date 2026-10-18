// 3D scanner turntable controller for ESP32-C6 (port of the Raspberry Pi app.py).

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "camera.hpp"
#include "config.h"
#include "motor.hpp"
#include "motor_tests.hpp"
#include "scanner.hpp"
#include "web_server.hpp"
#include "wifi.hpp"

namespace {
const char* TAG = "app";

void init_nvs()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}
}  // namespace

extern "C" void app_main(void)
{
    static Motor motor(cfg::MOTOR_ENABLE_PIN, cfg::MOTOR_STEP_PIN, cfg::MOTOR_DIR_PIN, cfg::MOTOR_STEP_DELAY_US);
    ESP_ERROR_CHECK(motor.init());

#if CONFIG_SCANNER_APP_MODE_MOTOR_TEST
    ESP_LOGW(TAG, "Firmware mode: motor test");
    run_motor_test(motor);
    return;
#elif CONFIG_SCANNER_APP_MODE_MOVE_TEST
    ESP_LOGW(TAG, "Firmware mode: move test");
    run_move_test(motor);
    return;
#endif

    init_nvs();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    static Camera camera(cfg::PHONE_SEND_TIMEOUT_MS, cfg::PHOTO_CONFIRM_TIMEOUT_MS);
    ESP_ERROR_CHECK(camera.init());

    static Scanner scanner(motor, camera,
                           ScannerConfig{
                               cfg::PHOTOS_PER_REVOLUTION,
                               cfg::MOTOR_STEPS_PER_REVOLUTION,
                               cfg::MOTOR_RESOLUTION,
                               cfg::PRE_CAPTURE_DELAY_MS,
                               cfg::POST_CAPTURE_DELAY_MS,
                           });
    ESP_ERROR_CHECK(scanner.init());

    static WebServer web(scanner, motor, camera, WebConfig{cfg::SERVER_PORT, cfg::WEBSOCKET_PORT, cfg::MOTOR_JOG});
    scanner.set_status_callback([](const ScanStatus& status) { web.broadcast_status(status); });

    ESP_ERROR_CHECK(wifi_start());
    if (mdns_start(cfg::SERVER_PORT, cfg::WEBSOCKET_PORT) != ESP_OK) {
        ESP_LOGW(TAG, "mDNS unavailable, use the IP address");
    }
    ESP_ERROR_CHECK(web.start());

    ESP_LOGI(TAG, "3D scanner ready");
}
