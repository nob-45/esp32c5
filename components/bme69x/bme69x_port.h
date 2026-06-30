/**
 * @file bme69x_port.h
 * @brief ESP-IDF 适配层 - 将 Bosch 官方 BME69x 驱动移植到 ESP-IDF
 *        替换 main/station_example_main.c 中手写的 BME690 驱动
 */
#ifndef BME69X_PORT_H
#define BME69X_PORT_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 默认 I2C 配置（与 factory_demo 示例一致：SDA=GPIO2, SCL=GPIO3, 100kHz）。
 * 如果用户实际接线不同，请在调用 bme69x_port_init 之前修改这几个宏 */
#ifndef BME69X_I2C_SDA_GPIO
#define BME69X_I2C_SDA_GPIO  2
#endif
#ifndef BME69X_I2C_SCL_GPIO
#define BME69X_I2C_SCL_GPIO  3
#endif
#ifndef BME69X_I2C_FREQ_HZ
#define BME69X_I2C_FREQ_HZ   100000   /* 100 kHz 与 SensairShuttle 示例一致 */
#endif
#ifndef BME69X_I2C_ADDR
#define BME69X_I2C_ADDR      0x76     /* BME69X_I2C_ADDR_LOW */
#endif

/* 初始化 I2C 总线 + Bosch BME69x 驱动。
 * 重复调用幂等（首次成功后不再重新初始化）。 */
esp_err_t bme69x_port_init(void);

/* 读取一次数据（forced 模式，单次测量），同时返回温度/湿度/压力/气体电阻。
 * 数据无效（gas_new_data=0 或 gas_valid=0）时返回 ESP_ERR_INVALID_STATE，
 * 调用方应继续读取直到成功为止。 */
esp_err_t bme69x_port_read(float *temperature_c,
                           float *humidity_pct,
                           float *pressure_hpa,
                           float *gas_resistance_ohm);

#ifdef __cplusplus
}
#endif

#endif /* BME69X_PORT_H */