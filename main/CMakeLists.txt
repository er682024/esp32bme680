idf_component_register(
    SRCS "main.c"
            "wifi.c"
            "http_server.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
	app_update
        bme680
        driver
        esp_event
        esp_netif
        esp_http_server
	esp_partition
        esp_timer
        esp_wifi
        mbedtls
        nvs_flash
)

