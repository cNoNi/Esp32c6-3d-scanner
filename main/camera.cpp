#include "camera.hpp"

#include "esp_check.h"
#include "esp_log.h"

namespace {
const char* TAG = "camera";

const char TAKE_PHOTO_CMD[] = "take_photo";

const char ERR_NOT_CONNECTED[] = "Telefon nie jest podłączony";
const char ERR_SEND_FAILED[] = "Nie udało się wysłać polecenia do telefonu";
const char ERR_NO_CONFIRMATION[] = "Telefon nie potwierdził zdjęcia";
}  // namespace

Camera::Camera(uint32_t send_timeout_ms, uint32_t confirm_timeout_ms)
    : send_timeout_ms_(send_timeout_ms), confirm_timeout_ms_(confirm_timeout_ms)
{
}

esp_err_t Camera::init()
{
    photo_done_ = xSemaphoreCreateBinary();
    send_result_ = xQueueCreate(1, sizeof(SendResult));
    ESP_RETURN_ON_FALSE(photo_done_ && send_result_, ESP_ERR_NO_MEM, TAG, "alloc failed");
    return ESP_OK;
}

void Camera::set_connection(httpd_handle_t server, int fd)
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    server_ = server;
    fd_ = fd;
    ESP_LOGI(TAG, "Camera: telefon podłączony (fd %d)", fd);
}

void Camera::disconnect(int fd)
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    // disconnect only if this is the current connection
    if (fd_ == fd) {
        server_ = nullptr;
        fd_ = -1;
        xSemaphoreTake(photo_done_, 0);
        ESP_LOGI(TAG, "Camera: telefon rozłączony");
    }
}

bool Camera::connected()
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return server_ != nullptr && fd_ >= 0;
}

void Camera::send_work(void* arg)
{
    // Runs in the WebSocket server task
    auto* job = static_cast<SendJob*>(arg);

    esp_err_t err = ESP_FAIL;
    if (httpd_ws_get_fd_info(job->server, job->fd) == HTTPD_WS_CLIENT_WEBSOCKET) {
        httpd_ws_frame_t frame = {};
        frame.final = true;
        frame.type = HTTPD_WS_TYPE_TEXT;
        frame.payload = reinterpret_cast<uint8_t*>(const_cast<char*>(TAKE_PHOTO_CMD));
        frame.len = sizeof(TAKE_PHOTO_CMD) - 1;
        err = httpd_ws_send_frame_async(job->server, job->fd, &frame);
    }

    SendResult result = {job->seq, err};
    xQueueOverwrite(job->camera->send_result_, &result);
    delete job;
}

bool Camera::wait_send_result(uint32_t seq, esp_err_t* err)
{
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(send_timeout_ms_);

    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        if (elapsed >= timeout) {
            return false;
        }
        SendResult result;
        if (xQueueReceive(send_result_, &result, timeout - elapsed) != pdTRUE) {
            return false;
        }
        // Ignore late results of earlier (timed out) requests
        if (result.seq == seq) {
            *err = result.err;
            return true;
        }
    }
}

bool Camera::take_photo(const char* filename, std::string* error)
{
    std::lock_guard<std::mutex> photo_lock(photo_mutex_);

    httpd_handle_t server;
    int fd;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        server = server_;
        fd = fd_;
    }

    if (server == nullptr || fd < 0) {
        *error = ERR_NOT_CONNECTED;
        return false;
    }

    xSemaphoreTake(photo_done_, 0);  // clear

    const uint32_t seq = ++seq_;
    auto* job = new SendJob{this, server, fd, seq};
    if (httpd_queue_work(server, &Camera::send_work, job) != ESP_OK) {
        delete job;
        *error = ERR_SEND_FAILED;
        return false;
    }

    esp_err_t send_err = ESP_FAIL;
    if (!wait_send_result(seq, &send_err) || send_err != ESP_OK) {
        ESP_LOGW(TAG, "Sending take_photo failed: %s", esp_err_to_name(send_err));
        *error = ERR_SEND_FAILED;
        return false;
    }

    if (xSemaphoreTake(photo_done_, pdMS_TO_TICKS(confirm_timeout_ms_)) != pdTRUE) {
        *error = ERR_NO_CONFIRMATION;
        return false;
    }

    ESP_LOGI(TAG, "Photo taken: %s", filename);
    return true;
}

void Camera::photo_taken()
{
    xSemaphoreGive(photo_done_);
}
