#pragma once

#include <cstdint>

#include "esp_err.h"

// Starts Wi-Fi in STA or AP mode (menuconfig). Non-blocking: STA keeps
// reconnecting in the background and logs the IP address once it gets one.
esp_err_t wifi_start();

// Advertises <hostname>.local plus the HTTP and WebSocket services
esp_err_t mdns_start(uint16_t http_port, uint16_t ws_port);
