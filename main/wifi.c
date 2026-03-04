#include "wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "freertos/event_groups.h"
#include <string.h>

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define MAX_RETRY          5

#define AP_SSID     "ESP32-BME680"
#define AP_PASSWORD "12345678"      // minimo 8 caratteri
#define AP_CHANNEL  1
#define AP_MAX_CONN 4

bool wifi_connected = false;
static const char *TAG = "WIFI";
static EventGroupHandle_t wifi_events;
static int retry_count = 0;

bool wifi_load_credentials(char *ssid, size_t ssid_len,
                            char *password, size_t pass_len) {
    nvs_handle_t nvs;
    if (nvs_open("wifi_cfg", NVS_READONLY, &nvs) != ESP_OK) return false;

    bool ok = (nvs_get_str(nvs, "ssid",     ssid,     &ssid_len) == ESP_OK &&
               nvs_get_str(nvs, "password", password, &pass_len) == ESP_OK);
    nvs_close(nvs);
    return ok;
}

static void event_handler(void *arg, esp_event_base_t base,
                           int32_t event_id, void *event_data) {
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < MAX_RETRY) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGW(TAG, "Riconnessione... (%d/%d)", retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "IP ottenuto: " IPSTR, IP2STR(&event->ip_info.ip));
        retry_count = 0;
        wifi_connected = true;
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "Client connesso all'AP - MAC: " MACSTR, MAC2STR(e->mac));
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *e = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "Client disconnesso dall'AP - MAC: " MACSTR, MAC2STR(e->mac));
    }
}

void wifi_init(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, event_handler, NULL);

    wifi_events = xEventGroupCreate();
}


void wifi_connect(const char *ssid, const char *password) {
    // Rimosso: nvs, netif, event_loop, wifi_init (già fatti in wifi_init)
    esp_netif_create_default_wifi_sta();

    wifi_config_t wifi_config = {};
    strncpy((char *)wifi_config.sta.ssid,     ssid,     sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
    esp_wifi_connect();

    ESP_LOGI(TAG, "Connessione a '%s'...", ssid);

    EventBits_t bits = xEventGroupWaitBits(wifi_events,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(10000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connesso!");
        wifi_connected = true;
    } else {
        ESP_LOGE(TAG, "Connessione fallita!");
        esp_wifi_stop();
    }
}

void wifi_start_ap(void) {
    ESP_LOGW(TAG, "Avvio modalità AP: SSID='%s' PASSWORD='%s'", AP_SSID, AP_PASSWORD);

    esp_netif_create_default_wifi_ap();  // ora funziona perché netif è già init

    wifi_config_t ap_config = {
        .ap = {
            .ssid           = AP_SSID,
            .ssid_len       = strlen(AP_SSID),
            .channel        = AP_CHANNEL,
            .password       = AP_PASSWORD,
            .max_connection = AP_MAX_CONN,
            .authmode       = WIFI_AUTH_WPA2_PSK,
        }
    };

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "AP attivo — connettiti a '%s' e apri http://192.168.4.1", AP_SSID);
}
