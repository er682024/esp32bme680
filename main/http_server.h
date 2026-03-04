#pragma once
#include "esp_http_server.h"
#include "bme680_wrapper.h"
#include <stdint.h>
void http_server_start(void);
void http_server_stop(void);
void http_server_update_data(float temp, float hum, float pres, uint32_t gas, const char *time_str);
