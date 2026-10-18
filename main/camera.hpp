#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// The "camera" is a phone connected over WebSocket.
// take_photo() sends "take_photo" and waits for the phone to answer "photo_taken".
// The photos are stored on the phone; the scanner only triggers them.
class Camera {
public:
    Camera(uint32_t send_timeout_ms, uint32_t confirm_timeout_ms);

    esp_err_t init();

    // Called by the WebSocket server on handshake / socket close
    void set_connection(httpd_handle_t server, int fd);
    void disconnect(int fd);  // only disconnects if fd is the current connection

    bool connected();

    // Blocking. Returns false and fills *error on failure.
    bool take_photo(const char* filename, std::string* error);

    // Called when the phone sends "photo_taken"
    void photo_taken();

private:
    struct SendJob {
        Camera* camera;
        httpd_handle_t server;
        int fd;
        uint32_t seq;
    };
    struct SendResult {
        uint32_t seq;
        esp_err_t err;
    };

    static void send_work(void* arg);
    bool wait_send_result(uint32_t seq, esp_err_t* err);

    uint32_t send_timeout_ms_;
    uint32_t confirm_timeout_ms_;

    std::mutex conn_mutex_;
    httpd_handle_t server_ = nullptr;
    int fd_ = -1;

    std::mutex photo_mutex_;  // one photo request at a time
    uint32_t seq_ = 0;
    SemaphoreHandle_t photo_done_ = nullptr;
    QueueHandle_t send_result_ = nullptr;
};
