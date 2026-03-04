#pragma once

#include "bme68x.h"
#include <stdbool.h>

void bme680_init_sensor(struct bme68x_dev *dev);
bool bme680_read_data(struct bme68x_dev *dev, struct bme68x_data *data);

