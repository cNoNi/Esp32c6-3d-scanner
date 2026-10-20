#include "web_server.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unistd.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/task.h"

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");
extern const char style_css_start[] asm("_binary_style_css_start");
extern const char style_css_end[] asm("_binary_style_css_end");

namespace {
const char* TAG = "web";

constexpr size_t MAX_SSE_CLIENTS = 4;
constexpr size_t MAX_WS_FRAME_LEN = 1024;
constexpr uint64_t SSE_PING_PERIOD_US = 15ULL * 1000 * 1000;
constexpr uint16_t HTTP_CTRL_PORT = 32768;
constexpr uint16_t WS_CTRL_PORT = 32769;

const char SSE_HEADERS[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "X-Accel-Buffering: no\r\n"
    "\r\n";

void json_append_string(std::string& out, const std::string& value)
{
    out += '"';
    for (char c : value) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
                out += buf;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

std::string status_to_json(const ScanStatus& status)
{
    std::string out = "{\"running\":";
    out += status.running ? "true" : "false";
    out += ",\"photo\":" + std::to_string(status.photo);
    out += ",\"total\":" + std::to_string(status.total);
    out += ",\"error\":";
    if (status.has_error) {
        json_append_string(out, status.error);
    } else {
        out += "null";
    }
    out += '}';
    return out;
}

std::string json_status(const char* status)
{
    std::string out = "{\"status\":";
    json_append_string(out, status);
    out += '}';
    return out;
}

esp_err_t send_json(httpd_req_t* req, const std::string& body, const char* http_status = nullptr)
{
    if (http_status) {
        httpd_resp_set_status(req, http_status);
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body.data(), static_cast<ssize_t>(body.size()));
}

bool socket_send_all(httpd_handle_t hd, int fd, const char* data, size_t len)
{
    while (len > 0) {
        int sent = httpd_socket_send(hd, fd, data, len, 0);
        if (sent <= 0) {
            return false;
        }
        data += sent;
        len -= static_cast<size_t>(sent);
    }
    return true;
}

esp_err_t register_uri(httpd_handle_t hd, const char* uri, httpd_method_t method,
                       esp_err_t (*handler)(httpd_req_t*), void* ctx, bool websocket = false)
{
    httpd_uri_t def = {};
    def.uri = uri;
    def.method = method;
    def.handler = handler;
    def.user_ctx = ctx;
    def.is_websocket = websocket;
    def.handle_ws_control_frames = false;
    return httpd_register_uri_handler(hd, &def);
}

void no_free(void*) {}

WebServer* self_of(httpd_req_t* req)
{
    return static_cast<WebServer*>(req->user_ctx);
}
}  // namespace

WebServer::WebServer(Scanner& scanner, Motor& motor, Camera& camera, const WebConfig& config)
    : scanner_(scanner), motor_(motor), camera_(camera), config_(config)
{
}

esp_err_t WebServer::start()
{
    photo_queue_ = xQueueCreate(2, sizeof(httpd_req_t*));
    ESP_RETURN_ON_FALSE(photo_queue_ != nullptr, ESP_ERR_NO_MEM, TAG, "queue alloc failed");
    ESP_RETURN_ON_FALSE(xTaskCreate(&WebServer::photo_worker, "photo_req", 4096, this, 5, nullptr) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "photo task create failed");

    ESP_RETURN_ON_ERROR(start_ws(), TAG, "WebSocket server start failed");
    ESP_RETURN_ON_ERROR(start_http(), TAG, "HTTP server start failed");

    esp_timer_create_args_t timer_args = {};
    timer_args.callback = &WebServer::sse_ping_timer;
    timer_args.arg = this;
    timer_args.name = "sse_ping";
    ESP_RETURN_ON_ERROR(esp_timer_create(&timer_args, &ping_timer_), TAG, "timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(ping_timer_, SSE_PING_PERIOD_US), TAG, "timer start failed");

    return ESP_OK;
}

// ---------------------------------------------------------------------------
// HTTP server (port 5000)
// ---------------------------------------------------------------------------

esp_err_t WebServer::start_http()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = config_.http_port;
    config.ctrl_port = HTTP_CTRL_PORT;
    config.max_open_sockets = 8;
    config.max_uri_handlers = 12;
    config.lru_purge_enable = true;
    config.stack_size = 6144;
    config.global_user_ctx = this;
    config.global_user_ctx_free_fn = no_free;
    config.close_fn = &WebServer::on_http_close;

    ESP_RETURN_ON_ERROR(httpd_start(&http_, &config), TAG, "httpd_start failed");

    ESP_RETURN_ON_ERROR(register_uri(http_, "/", HTTP_GET, handle_index, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/static/style.css", HTTP_GET, handle_style, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/scan/start", HTTP_POST, handle_scan_start, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/scan/stop", HTTP_POST, handle_scan_stop, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/scan/status", HTTP_GET, handle_scan_status, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/camera/photo", HTTP_POST, handle_camera_photo, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/motor/cw", HTTP_POST, handle_motor_cw, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/motor/ccw", HTTP_POST, handle_motor_ccw, this), TAG, "uri");
    ESP_RETURN_ON_ERROR(register_uri(http_, "/events", HTTP_GET, handle_events, this), TAG, "uri");

    ESP_LOGI(TAG, "HTTP server on port %u", config_.http_port);
    return ESP_OK;
}

esp_err_t WebServer::handle_index(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // EMBED_TXTFILES appends a NUL terminator
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

esp_err_t WebServer::handle_style(httpd_req_t* req)
{
    httpd_resp_set_type(req, "text/css");
    return httpd_resp_send(req, style_css_start, style_css_end - style_css_start - 1);
}

esp_err_t WebServer::handle_scan_start(httpd_req_t* req)
{
    WebServer* self = self_of(req);
    if (!self->scanner_.start()) {
        return send_json(req, json_status("already running"));
    }
    return send_json(req, json_status("started"));
}

esp_err_t WebServer::handle_scan_stop(httpd_req_t* req)
{
    self_of(req)->scanner_.stop();
    return send_json(req, json_status("stopping"));
}

esp_err_t WebServer::handle_scan_status(httpd_req_t* req)
{
    return send_json(req, status_to_json(self_of(req)->scanner_.status()));
}

esp_err_t WebServer::handle_camera_photo(httpd_req_t* req)
{
    WebServer* self = self_of(req);

    httpd_req_t* async_req = nullptr;
    ESP_RETURN_ON_ERROR(httpd_req_async_handler_begin(req, &async_req), TAG, "async begin failed");

    if (xQueueSend(self->photo_queue_, &async_req, 0) != pdTRUE) {
        std::string body = "{\"status\":\"error\",\"message\":";
        json_append_string(body, "busy");
        body += '}';
        send_json(async_req, body, "409 Conflict");
        httpd_req_async_handler_complete(async_req);
    }
    return ESP_OK;
}

void WebServer::photo_worker(void* arg)
{
    auto* self = static_cast<WebServer*>(arg);

    for (;;) {
        httpd_req_t* req = nullptr;
        xQueueReceive(self->photo_queue_, &req, portMAX_DELAY);

        std::string error;
        if (self->camera_.take_photo("manual.jpg", &error)) {
            send_json(req, json_status("photo_taken"));
        } else {
            std::string body = "{\"status\":\"error\",\"message\":";
            json_append_string(body, error);
            body += '}';
            send_json(req, body, "400 Bad Request");
        }
        httpd_req_async_handler_complete(req);
    }
}

esp_err_t WebServer::jog(httpd_req_t* req, int direction)
{
    if (scanner_.running()) {
        return send_json(req, json_status("busy"), "409 Conflict");
    }

    std::unique_lock<std::mutex> lock(motor_.mutex(), std::try_to_lock);
    if (!lock.owns_lock()) {
        return send_json(req, json_status("busy"), "409 Conflict");
    }

    motor_.set_motor_enabled(true);
    motor_.run_to_position_steps(motor_.position() + direction * config_.jog_steps);
    motor_.set_motor_enabled(false);

    return send_json(req, json_status("ok"));
}

esp_err_t WebServer::handle_motor_cw(httpd_req_t* req)
{
    return self_of(req)->jog(req, 1);
}

esp_err_t WebServer::handle_motor_ccw(httpd_req_t* req)
{
    return self_of(req)->jog(req, -1);
}

// ---------------------------------------------------------------------------
// Server-Sent Events (/events)
//
// The handler writes the SSE headers itself and returns without a regular
// response, leaving the socket open. Later events are written to the socket
// from httpd work items (always in the HTTP server task).
// ---------------------------------------------------------------------------

esp_err_t WebServer::handle_events(httpd_req_t* req)
{
    WebServer* self = self_of(req);
    const int fd = httpd_req_to_sockfd(req);

    if (self->sse_fds_.size() >= MAX_SSE_CLIENTS) {
        // drop the oldest client (usually a stale browser tab)
        const int oldest = self->sse_fds_.front();
        self->remove_sse_fd(oldest);
        httpd_sess_trigger_close(self->http_, oldest);
    }

    if (httpd_send(req, SSE_HEADERS, sizeof(SSE_HEADERS) - 1) < 0) {
        return ESP_FAIL;
    }

    const std::string first = "data: " + status_to_json(self->scanner_.status()) + "\n\n";
    if (httpd_send(req, first.data(), first.size()) < 0) {
        return ESP_FAIL;
    }

    self->sse_fds_.push_back(fd);
    ESP_LOGI(TAG, "SSE client connected (fd %d, %u total)", fd, static_cast<unsigned>(self->sse_fds_.size()));
    return ESP_OK;
}

void WebServer::broadcast_status(const ScanStatus& status)
{
    if (http_ == nullptr) {
        return;
    }
    auto* job = new SseJob{this, "data: " + status_to_json(status) + "\n\n"};
    if (httpd_queue_work(http_, &WebServer::sse_send_work, job) != ESP_OK) {
        delete job;
    }
}

void WebServer::sse_ping_timer(void* arg)
{
    // SSE comment line: keeps proxies happy and detects dead clients
    auto* self = static_cast<WebServer*>(arg);
    auto* job = new SseJob{self, ": ping\n\n"};
    if (httpd_queue_work(self->http_, &WebServer::sse_send_work, job) != ESP_OK) {
        delete job;
    }
}

void WebServer::sse_send_work(void* arg)
{
    auto* job = static_cast<SseJob*>(arg);
    job->server->sse_send_all(job->payload);
    delete job;
}

void WebServer::sse_send_all(const std::string& data)
{
    std::vector<int> dead;
    for (int fd : sse_fds_) {
        if (!socket_send_all(http_, fd, data.data(), data.size())) {
            dead.push_back(fd);
        }
    }
    for (int fd : dead) {
        remove_sse_fd(fd);
        httpd_sess_trigger_close(http_, fd);
    }
}

void WebServer::remove_sse_fd(int fd)
{
    sse_fds_.erase(std::remove(sse_fds_.begin(), sse_fds_.end(), fd), sse_fds_.end());
}

void WebServer::on_http_close(httpd_handle_t hd, int fd)
{
    auto* self = static_cast<WebServer*>(httpd_get_global_user_ctx(hd));
    self->remove_sse_fd(fd);
    close(fd);
}

// ---------------------------------------------------------------------------
// Phone WebSocket server (port 8765)
// ---------------------------------------------------------------------------

esp_err_t WebServer::start_ws()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = config_.ws_port;
    config.ctrl_port = WS_CTRL_PORT;
    config.max_open_sockets = 3;
    config.max_uri_handlers = 2;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;  // accept any path, like websockets.serve
    config.global_user_ctx = this;
    config.global_user_ctx_free_fn = no_free;
    config.close_fn = &WebServer::on_ws_close;

    ESP_RETURN_ON_ERROR(httpd_start(&ws_, &config), TAG, "httpd_start (ws) failed");
    ESP_RETURN_ON_ERROR(register_uri(ws_, "/*", HTTP_GET, handle_ws, this, true), TAG, "uri");

    ESP_LOGI(TAG, "WebSocket działa na porcie %u", config_.ws_port);
    return ESP_OK;
}

esp_err_t WebServer::handle_ws(httpd_req_t* req)
{
    WebServer* self = self_of(req);

    if (req->method == HTTP_GET) {
        // handshake done
        self->camera_.set_connection(req->handle, httpd_req_to_sockfd(req));
        ESP_LOGI(TAG, "Telefon połączony");
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {};
    frame.type = HTTPD_WS_TYPE_TEXT;
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &frame, 0), TAG, "ws frame length read failed");

    if (frame.len == 0) {
        return ESP_OK;
    }
    if (frame.len > MAX_WS_FRAME_LEN) {
        ESP_LOGW(TAG, "WebSocket frame too large (%u bytes)", static_cast<unsigned>(frame.len));
        return ESP_FAIL;  // closes the connection
    }

    std::string message(frame.len, '\0');
    frame.payload = reinterpret_cast<uint8_t*>(&message[0]);
    ESP_RETURN_ON_ERROR(httpd_ws_recv_frame(req, &frame, frame.len), TAG, "ws frame read failed");

    if (frame.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Telefon: %s", message.c_str());
    self->camera_.ensure_connection(req->handle, httpd_req_to_sockfd(req));

    if (message == "photo_taken") {
        self->camera_.photo_taken();
    }
    return ESP_OK;
}

void WebServer::on_ws_close(httpd_handle_t hd, int fd)
{
    auto* self = static_cast<WebServer*>(httpd_get_global_user_ctx(hd));
    self->camera_.disconnect(fd);
    close(fd);
}
