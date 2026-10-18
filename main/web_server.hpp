#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "camera.hpp"
#include "motor.hpp"
#include "scanner.hpp"

struct WebConfig {
    uint16_t http_port;  // UI, REST API, Server-Sent Events
    uint16_t ws_port;    // phone WebSocket
    int32_t jog_steps;
};

// Port of app.py: Flask routes + SSE on http_port, phone WebSocket on ws_port.
class WebServer {
public:
    WebServer(Scanner& scanner, Motor& motor, Camera& camera, const WebConfig& config);

    esp_err_t start();

    // Thread-safe: pushes the status to every /events client
    void broadcast_status(const ScanStatus& status);

private:
    struct SseJob {
        WebServer* server;
        std::string payload;
    };

    esp_err_t start_http();
    esp_err_t start_ws();

    // HTTP routes
    static esp_err_t handle_index(httpd_req_t* req);
    static esp_err_t handle_style(httpd_req_t* req);
    static esp_err_t handle_scan_start(httpd_req_t* req);
    static esp_err_t handle_scan_stop(httpd_req_t* req);
    static esp_err_t handle_scan_status(httpd_req_t* req);
    static esp_err_t handle_camera_photo(httpd_req_t* req);
    static esp_err_t handle_motor_cw(httpd_req_t* req);
    static esp_err_t handle_motor_ccw(httpd_req_t* req);
    static esp_err_t handle_events(httpd_req_t* req);
    esp_err_t jog(httpd_req_t* req, int direction);

    // Phone WebSocket
    static esp_err_t handle_ws(httpd_req_t* req);

    static void on_http_close(httpd_handle_t hd, int fd);
    static void on_ws_close(httpd_handle_t hd, int fd);

    // /camera/photo blocks up to ~15 s, so it is answered from a worker task
    static void photo_worker(void* arg);

    // SSE; these run in the HTTP server task only, so sse_fds_ needs no lock
    static void sse_send_work(void* arg);
    static void sse_ping_timer(void* arg);
    void sse_send_all(const std::string& data);
    void remove_sse_fd(int fd);

    Scanner& scanner_;
    Motor& motor_;
    Camera& camera_;
    WebConfig config_;

    httpd_handle_t http_ = nullptr;
    httpd_handle_t ws_ = nullptr;

    QueueHandle_t photo_queue_ = nullptr;
    esp_timer_handle_t ping_timer_ = nullptr;
    std::vector<int> sse_fds_;
};
