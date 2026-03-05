#include "driver/i2c.h"
#include "bme680_wrapper.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "time.h"
#include "wifi.h"
#include "http_server.h"

extern bool wifi_connected;

#define SDA_PIN 21
#define SCL_PIN 22

static const char *TAG = "MAIN";

// Prototipi
void time_sync_init(void);
void get_time_str(char *buf, size_t len);
void i2c_scan(void);

void app_main(void)
{
    wifi_init();
    
    char ssid[64]     = {0};
    char password[64] = {0};
    
    if (wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        ESP_LOGI(TAG, "Credenziali trovate: SSID='%s'", ssid);
        wifi_connect(ssid, password);
    } else {
        ESP_LOGW(TAG, "Nessuna credenziale salvata");
    }
    
    
    if (wifi_connected) {
        time_sync_init();
        http_server_start();   // ← avvia server anche in modalità STA
    } else {
        // Avvia AP + pagina di configurazione
        wifi_start_ap();
        http_server_start();
        // Resta in attesa — il reboot avverrà dopo il salvataggio
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA_PIN,
        .scl_io_num = SCL_PIN,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000
    };

    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
    // i2c_scan(); 

    struct bme68x_dev dev;
    struct bme68x_data data;
    bme680_init_sensor(&dev);

    while (1) {
        if (bme680_read_data(&dev, &data)) {
            char time_str[32];
            get_time_str(time_str, sizeof(time_str));
            
            // Legge RSSI WiFi (valido solo in modalità STA)
            int8_t rssi = 0;
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                rssi = ap_info.rssi;
            }
            
            http_server_update_data(         // ← aggiorna i dati per la pagina
            (float)data.temperature,
            (float)data.humidity,
            (float)(data.pressure / 100.0),
            (uint32_t)data.gas_resistance,
            time_str);
            
            ESP_LOGI(TAG,
                "[%s] T=%.2f°C  H=%.2f%%  P=%.2f hPa  Gas=%u ohm  WiFi=%d dBm",
                time_str,
                (float)data.temperature,
                (float)data.humidity,
                (float)(data.pressure / 100.0),
                (unsigned)data.gas_resistance,
                (int)rssi);
        }
        vTaskDelay(2000 / portTICK_PERIOD_MS);
    }
}

void i2c_scan(void) {
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t ret = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(10));
        i2c_cmd_link_delete(cmd);
        if (ret == ESP_OK) {
            ESP_LOGI("I2C_SCAN", "Device found at 0x%02X", addr);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void time_sync_init(void) {
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    int retry = 0;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && retry++ < 20) {
        ESP_LOGI("NTP", "Attesa sincronizzazione... (%d/20)", retry);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void get_time_str(char *buf, size_t len) {
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);
    strftime(buf, len, "%d/%m/%Y %H:%M:%S", &timeinfo);
}
