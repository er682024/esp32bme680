#include "bme680_wrapper.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include <string.h>

#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_TIMEOUT_MS 1000

static const char *TAG = "BME680";

static int8_t i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    uint8_t dev_addr = *(uint8_t *)intf_ptr;

    return i2c_master_write_read_device(
        I2C_MASTER_NUM,
        dev_addr,
        &reg_addr,
        1,
        reg_data,
        len,
        I2C_TIMEOUT_MS / portTICK_PERIOD_MS
    );
}

static int8_t i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    uint8_t dev_addr = *(uint8_t *)intf_ptr;

    uint8_t buf[len + 1];
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);

    return i2c_master_write_to_device(
        I2C_MASTER_NUM,
        dev_addr,
        buf,
        len + 1,
        I2C_TIMEOUT_MS / portTICK_PERIOD_MS
    );
}

static void delay_us(uint32_t period, void *intf_ptr)
{
    esp_rom_delay_us(period);
}

void bme680_init_sensor(struct bme68x_dev *dev)
{
    static uint8_t dev_addr = BME68X_I2C_ADDR_HIGH;

    dev->intf = BME68X_I2C_INTF;
    dev->read = i2c_read;
    dev->write = i2c_write;
    dev->delay_us = delay_us;
    dev->intf_ptr = &dev_addr;
    dev->amb_temp = 25;

    int8_t rslt = bme68x_init(dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "Errore inizializzazione BME680: %d", rslt);
    }
}

bool bme680_read_data(struct bme68x_dev *dev, struct bme68x_data *data)
{
    struct bme68x_conf conf = {
        .filter = BME68X_FILTER_OFF,
        .odr = BME68X_ODR_NONE,
        .os_hum = BME68X_OS_2X,
        .os_pres = BME68X_OS_4X,
        .os_temp = BME68X_OS_8X
    };

    struct bme68x_heatr_conf heatr_conf = {
        .enable = BME68X_ENABLE,
        .heatr_temp = 320,
        .heatr_dur = 150
    };

    bme68x_set_conf(&conf, dev);
    bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_conf, dev);
    bme68x_set_op_mode(BME68X_FORCED_MODE, dev);
    
    uint32_t dur = bme68x_get_meas_dur(BME68X_FORCED_MODE, &conf, dev);
    vTaskDelay(pdMS_TO_TICKS(dur));

    uint8_t n_fields;
    return bme68x_get_data(BME68X_FORCED_MODE, data, &n_fields, dev) == BME68X_OK;
}

