#include "wifi.hpp"

#include <cstring>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "mdns.h"
#include "sdkconfig.h"

namespace {
const char* TAG = "wifi";

void on_wifi_event(void*, esp_event_base_t base, int32_t id, void* data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* event = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGW(TAG, "Disconnected (reason %d), reconnecting...", event->reason);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(data);
        ESP_LOGI(TAG, "Connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "Access point started");
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client joined the access point");
    }
}
}  // namespace

esp_err_t wifi_start()
{
#if CONFIG_SCANNER_WIFI_MODE_AP
    esp_netif_t* netif = esp_netif_create_default_wifi_ap();
#else
    esp_netif_t* netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(netif, CONFIG_SCANNER_HOSTNAME);
#endif
    ESP_RETURN_ON_FALSE(netif != nullptr, ESP_FAIL, TAG, "netif create failed");

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, nullptr),
                        TAG, "event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, nullptr),
                        TAG, "event register failed");

    wifi_config_t wifi_cfg = {};

#if CONFIG_SCANNER_WIFI_MODE_AP
    strlcpy(reinterpret_cast<char*>(wifi_cfg.ap.ssid), CONFIG_SCANNER_AP_SSID, sizeof(wifi_cfg.ap.ssid));
    strlcpy(reinterpret_cast<char*>(wifi_cfg.ap.password), CONFIG_SCANNER_AP_PASSWORD, sizeof(wifi_cfg.ap.password));
    wifi_cfg.ap.ssid_len = static_cast<uint8_t>(strlen(CONFIG_SCANNER_AP_SSID));
    wifi_cfg.ap.channel = CONFIG_SCANNER_AP_CHANNEL;
    wifi_cfg.ap.max_connection = 4;
    wifi_cfg.ap.authmode = strlen(CONFIG_SCANNER_AP_PASSWORD) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_cfg), TAG, "set config failed");
    ESP_LOGI(TAG, "Starting access point \"%s\" (IP 192.168.4.1)", CONFIG_SCANNER_AP_SSID);
#else
    strlcpy(reinterpret_cast<char*>(wifi_cfg.sta.ssid), CONFIG_SCANNER_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy(reinterpret_cast<char*>(wifi_cfg.sta.password), CONFIG_SCANNER_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg), TAG, "set config failed");
    ESP_LOGI(TAG, "Connecting to \"%s\"...", CONFIG_SCANNER_WIFI_SSID);
#endif

    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    // No modem sleep: keeps WebSocket/HTTP latency low
    esp_wifi_set_ps(WIFI_PS_NONE);
    return ESP_OK;
}

esp_err_t mdns_start(uint16_t http_port, uint16_t ws_port)
{
    ESP_RETURN_ON_ERROR(mdns_init(), TAG, "mdns init failed");
    mdns_hostname_set(CONFIG_SCANNER_HOSTNAME);
    mdns_instance_name_set("3D Scanner");
    mdns_service_add(nullptr, "_http", "_tcp", http_port, nullptr, 0);
    mdns_service_add(nullptr, "_ws", "_tcp", ws_port, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: http://%s.local:%u  ws://%s.local:%u", CONFIG_SCANNER_HOSTNAME, http_port,
             CONFIG_SCANNER_HOSTNAME, ws_port);
    return ESP_OK;
}
