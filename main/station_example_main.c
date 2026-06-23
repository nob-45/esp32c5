/* ESP32-C5 Wi-Fi Station + BME690 + OneNET MQTT
 *
 * 功能：
 * 1. 连接 Wi-Fi；
 * 2. 通过 I2C 自动扫描 BME690 地址 0x76 / 0x77；
 * 3. 读取 BME690 真实温度、湿度、气压、气体电阻数据；
 * 4. 通过 MQTT 上报到 OneNET 物模型属性上报主题。
 *
 * 硬件默认连接：
 * ESP32-SensairShuttle 的 BME690 子板插在 Shuttle Board Connector 上。
 * 官方默认 I2C：
 * BME690 SDA -> ESP32-C5 GPIO2
 * BME690 SCL -> ESP32-C5 GPIO3
 * I2C 频率 -> 100 kHz
 */

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "mqtt_client.h"
#include "nvs_flash.h"

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "lwip/err.h"
#include "lwip/sys.h"

#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include <time.h>
#include <sys/time.h>

#include "volc_conv_app.h"

/* Wi-Fi 配置来自 idf.py menuconfig -> Example Configuration */
#define EXAMPLE_ESP_WIFI_SSID CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_MAXIMUM_RETRY CONFIG_ESP_MAXIMUM_RETRY

#if CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HUNT_AND_PECK
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HUNT_AND_PECK
#define EXAMPLE_H2E_IDENTIFIER ""
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_HASH_TO_ELEMENT
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_HASH_TO_ELEMENT
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#elif CONFIG_ESP_STATION_EXAMPLE_WPA3_SAE_PWE_BOTH
#define ESP_WIFI_SAE_MODE WPA3_SAE_PWE_BOTH
#define EXAMPLE_H2E_IDENTIFIER CONFIG_ESP_WIFI_PW_ID
#endif

#if CONFIG_ESP_WIFI_AUTH_OPEN
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_OPEN
#elif CONFIG_ESP_WIFI_AUTH_WEP
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WEP
#elif CONFIG_ESP_WIFI_AUTH_WPA_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA_WPA2_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA_WPA2_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WPA2_WPA3_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WPA2_WPA3_PSK
#elif CONFIG_ESP_WIFI_AUTH_WAPI_PSK
#define ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD WIFI_AUTH_WAPI_PSK
#endif

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define MQTT_CONNECTED_BIT BIT0

#define I2C_MASTER_PORT I2C_NUM_0
#define BME690_I2C_ADDR_LOW 0x76
#define BME690_I2C_ADDR_HIGH 0x77
#define BME690_CHIP_ID_REG 0xD0
#define BME690_CHIP_ID 0x61
#define BME690_RESET_REG 0xE0
#define BME690_SOFT_RESET_CMD 0xB6

#define BME690_REG_CTRL_HUM 0x72
#define BME690_REG_CTRL_MEAS 0x74
#define BME690_REG_CONFIG 0x75
#define BME690_REG_CTRL_GAS_1 0x71
#define BME690_REG_GAS_WAIT_0 0x64
#define BME690_REG_RES_HEAT_0 0x5A
#define BME690_REG_FIELD0 0x1D

#define BME690_REG_COEFF1 0x89
#define BME690_LEN_COEFF1 25
#define BME690_REG_COEFF2 0xE1
#define BME690_LEN_COEFF2 16

#define BME690_REG_RES_HEAT_RANGE 0x02
#define BME690_REG_RES_HEAT_VAL 0x00
#define BME690_REG_RANGE_SW_ERR 0x04
#define BME690_REG_VARIANT_ID 0xF0

#define BME690_VARIANT_GAS_LOW 0x00
#define BME690_VARIANT_GAS_HIGH 0x01

#define BME690_I2C_XFER_TIMEOUT_MS 1000
#define BME690_I2C_RETRY_COUNT 3
#define BME690_I2C_RETRY_DELAY_MS 20
#define BME690_I2C_SCL_WAIT_US 20000

#define ONENET_TOKEN_VERSION "2018-10-31"
#define ONENET_TOKEN_METHOD "sha1"

#define ONENET_MODEL_TEMPERATURE_MIN (-40.0f)
#define ONENET_MODEL_TEMPERATURE_MAX 85.0f
#define ONENET_MODEL_HUMIDITY_MIN 0.0f
#define ONENET_MODEL_HUMIDITY_MAX 100.0f
#define ONENET_MODEL_PRESSURE_MIN 300.0f
#define ONENET_MODEL_PRESSURE_MAX 1100.0f
#define ONENET_MODEL_GAS_RESISTANCE_MIN 0.0f
#define ONENET_MODEL_GAS_RESISTANCE_MAX 10000000.0f

static const char *TAG = "esp32c5_bme690_onenet";

static EventGroupHandle_t s_wifi_event_group;
static EventGroupHandle_t s_mqtt_event_group;

static int s_retry_num = 0;
static bool s_wifi_connected = false;
static esp_mqtt_client_handle_t s_mqtt_client = NULL;
static char s_onenet_mqtt_password[384] = {0};

static i2c_master_bus_handle_t s_i2c_bus = NULL;
static i2c_master_dev_handle_t s_bme690_dev = NULL;
static uint8_t s_bme690_addr = 0;
static uint8_t s_bme690_variant_id = BME690_VARIANT_GAS_LOW;

/* BME690/BME680 校准参数。BME690 的基础环境数据寄存器与 BME680 兼容。 */
typedef struct {
    uint16_t par_t1;
    int16_t par_t2;
    int8_t par_t3;

    uint16_t par_p1;
    int16_t par_p2;
    int8_t par_p3;
    int16_t par_p4;
    int16_t par_p5;
    int8_t par_p6;
    int8_t par_p7;
    int16_t par_p8;
    int16_t par_p9;
    uint8_t par_p10;

    uint16_t par_h1;
    uint16_t par_h2;
    int8_t par_h3;
    int8_t par_h4;
    int8_t par_h5;
    uint8_t par_h6;
    int8_t par_h7;

    int8_t par_gh1;
    int16_t par_gh2;
    int8_t par_gh3;

    uint8_t res_heat_range;
    int8_t res_heat_val;
    int8_t range_sw_err;

    double t_fine;
} bme690_calib_data_t;

typedef struct {
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    float gas_resistance_ohm;
    bool gas_valid;
    bool heat_stable;
} bme690_values_t;

static bme690_calib_data_t s_bme690_calib;

/* ---------------- Wi-Fi ---------------- */

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_wifi_connected = false;
        if (s_retry_num < EXAMPLE_ESP_MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "正在重连 Wi-Fi...");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG, "Wi-Fi 连接失败");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "已获取 IP：" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_wifi_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .threshold.authmode = ESP_WIFI_SCAN_AUTH_MODE_THRESHOLD,
            .sae_pwe_h2e = ESP_WIFI_SAE_MODE,
            .sae_h2e_identifier = EXAMPLE_H2E_IDENTIFIER,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 关闭 WiFi 省电模式：实时音频持续上行时，modem sleep 会导致 TLS 发送
     * 阻塞（transport_poll_write 超时 → WS 周期性断连重连）。
     * 实时语音场景必须用 WIFI_PS_NONE。 */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Wi-Fi 初始化完成，正在连接 SSID：%s", EXAMPLE_ESP_WIFI_SSID);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        s_wifi_connected = true;
        ESP_LOGI(TAG, "Wi-Fi 已连接");
        return true;
    }

    s_wifi_connected = false;
    ESP_LOGE(TAG, "Wi-Fi 连接失败，请检查 SSID/密码/2.4GHz 网络");
    return false;
}

/* ---------------- I2C + BME690 基础读写 ---------------- */

static void bme690_recover_i2c_bus(const char *operation, esp_err_t reason)
{
    ESP_LOGW(TAG, "%s 失败：%s，延时后重试 I2C 传输", operation, esp_err_to_name(reason));

    /* ESP-IDF 新 I2C master 驱动在 GPIO2/GPIO3 已被 I2C 外设占用时执行 bus_reset，
     * 可能打印 “GPIO x is not usable, maybe conflict with others”。这里不再主动 reset，
     * 只延时后重试，避免无实际故障时出现误导性警告。
     */
    vTaskDelay(pdMS_TO_TICKS(BME690_I2C_RETRY_DELAY_MS));
}

static esp_err_t bme690_write_reg(uint8_t reg_addr, uint8_t value)
{
    uint8_t write_buf[2] = {reg_addr, value};
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 1; attempt <= BME690_I2C_RETRY_COUNT; attempt++) {
        ret = i2c_master_transmit(s_bme690_dev, write_buf, sizeof(write_buf), BME690_I2C_XFER_TIMEOUT_MS);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "写 BME690 寄存器 0x%02X 第 %d/%d 次失败", reg_addr, attempt, BME690_I2C_RETRY_COUNT);
        bme690_recover_i2c_bus("写 BME690 寄存器", ret);
    }

    return ret;
}

static esp_err_t bme690_read_regs(uint8_t reg_addr, uint8_t *data, size_t len)
{
    esp_err_t ret = ESP_FAIL;

    for (int attempt = 1; attempt <= BME690_I2C_RETRY_COUNT; attempt++) {
        ret = i2c_master_transmit_receive(s_bme690_dev,
                                          &reg_addr,
                                          1,
                                          data,
                                          len,
                                          BME690_I2C_XFER_TIMEOUT_MS);
        if (ret == ESP_OK) {
            return ESP_OK;
        }

        ESP_LOGW(TAG, "读 BME690 寄存器 0x%02X 第 %d/%d 次失败", reg_addr, attempt, BME690_I2C_RETRY_COUNT);
        bme690_recover_i2c_bus("读 BME690 寄存器", ret);
    }

    return ret;
}

static uint16_t u16_from_bytes(uint8_t lsb, uint8_t msb)
{
    return (uint16_t)(((uint16_t)msb << 8) | lsb);
}

static int16_t s16_from_bytes(uint8_t lsb, uint8_t msb)
{
    return (int16_t)(((uint16_t)msb << 8) | lsb);
}

static esp_err_t i2c_bus_init(void)
{
    if (CONFIG_BME690_I2C_SDA_GPIO < 0 || CONFIG_BME690_I2C_SCL_GPIO < 0 ||
        CONFIG_BME690_I2C_SDA_GPIO >= 64 || CONFIG_BME690_I2C_SCL_GPIO >= 64 ||
        CONFIG_BME690_I2C_SDA_GPIO == CONFIG_BME690_I2C_SCL_GPIO) {
        ESP_LOGE(TAG,
                 "I2C GPIO 配置不合理。当前 SDA=GPIO%d, SCL=GPIO%d；ESP32-SensairShuttle 默认 SDA=GPIO2, SCL=GPIO3。",
                 CONFIG_BME690_I2C_SDA_GPIO,
                 CONFIG_BME690_I2C_SCL_GPIO);
        return ESP_ERR_INVALID_ARG;
    }

    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .scl_io_num = CONFIG_BME690_I2C_SCL_GPIO,
        .sda_io_num = CONFIG_BME690_I2C_SDA_GPIO,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    ESP_LOGI(TAG, "初始化 I2C：SDA=GPIO%d, SCL=GPIO%d, freq=%d Hz",
             CONFIG_BME690_I2C_SDA_GPIO,
             CONFIG_BME690_I2C_SCL_GPIO,
             CONFIG_BME690_I2C_FREQ_HZ);

    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

static esp_err_t bme690_add_device(uint8_t address)
{
    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = address,
        .scl_speed_hz = CONFIG_BME690_I2C_FREQ_HZ,
        .scl_wait_us = BME690_I2C_SCL_WAIT_US,
    };

    return i2c_master_bus_add_device(s_i2c_bus, &dev_config, &s_bme690_dev);
}

static esp_err_t bme690_probe_and_add(void)
{
    const uint8_t addresses[] = {BME690_I2C_ADDR_LOW, BME690_I2C_ADDR_HIGH};

    for (size_t i = 0; i < sizeof(addresses); i++) {
        uint8_t addr = addresses[i];
        esp_err_t ret = i2c_master_probe(s_i2c_bus, addr, BME690_I2C_XFER_TIMEOUT_MS);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "I2C 地址 0x%02X 探测失败：%s", addr, esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(BME690_I2C_RETRY_DELAY_MS));
            continue;
        }

        ESP_LOGI(TAG, "I2C 扫描到设备地址：0x%02X，开始读取 Chip ID 确认设备", addr);
        vTaskDelay(pdMS_TO_TICKS(BME690_I2C_RETRY_DELAY_MS));

        ESP_RETURN_ON_ERROR(bme690_add_device(addr), TAG, "添加 BME690 I2C 设备失败");
        s_bme690_addr = addr;

        uint8_t chip_id = 0;
        ret = bme690_read_regs(BME690_CHIP_ID_REG, &chip_id, 1);
        if (ret == ESP_OK && chip_id == BME690_CHIP_ID) {
            ESP_LOGI(TAG, "确认 BME690/BME680：I2C 地址=0x%02X, Chip ID=0x%02X", s_bme690_addr, chip_id);
            return ESP_OK;
        }

        if (ret == ESP_OK) {
            ESP_LOGW(TAG, "地址 0x%02X 的 Chip ID 不匹配：读到 0x%02X，期望 0x%02X", addr, chip_id, BME690_CHIP_ID);
        } else {
            ESP_LOGW(TAG, "地址 0x%02X 读取 Chip ID 失败：%s", addr, esp_err_to_name(ret));
        }

        ESP_ERROR_CHECK_WITHOUT_ABORT(i2c_master_bus_rm_device(s_bme690_dev));
        s_bme690_dev = NULL;
        s_bme690_addr = 0;
        vTaskDelay(pdMS_TO_TICKS(BME690_I2C_RETRY_DELAY_MS));
    }

    ESP_LOGE(TAG, "没有在 0x76/0x77 确认到 BME690，请检查 SDA/SCL/VCC/GND 接线和传感器地址");
    return ESP_ERR_NOT_FOUND;
}

static uint8_t bme690_calc_heater_res(uint16_t target_temp_c, int16_t ambient_temp_c)
{
    if (target_temp_c > 400) {
        target_temp_c = 400;
    }

    int32_t var1 = (((int32_t)ambient_temp_c * s_bme690_calib.par_gh3) / 1000) * 256;
    int32_t var2 = (s_bme690_calib.par_gh1 + 784) *
                   (((((s_bme690_calib.par_gh2 + 154009) * target_temp_c * 5) / 100) + 3276800) / 10);
    int32_t var3 = var1 + (var2 / 2);
    int32_t var4 = var3 / (s_bme690_calib.res_heat_range + 4);
    int32_t var5 = (131 * s_bme690_calib.res_heat_val) + 65536;
    int32_t heatr_res_x100 = ((var4 / var5) - 250) * 34;
    int32_t heatr_res = (heatr_res_x100 + 50) / 100;

    if (heatr_res < 0) {
        heatr_res = 0;
    } else if (heatr_res > 255) {
        heatr_res = 255;
    }

    return (uint8_t)heatr_res;
}

static uint8_t bme690_calc_gas_wait(uint16_t duration_ms)
{
    uint8_t factor = 0;

    if (duration_ms >= 0xFC0) {
        return 0xFF;
    }

    while (duration_ms > 0x3F) {
        duration_ms /= 4;
        factor++;
    }

    return (uint8_t)(duration_ms + (factor * 64));
}

static esp_err_t bme690_read_calibration(void)
{
    uint8_t coeff[BME690_LEN_COEFF1 + BME690_LEN_COEFF2] = {0};
    uint8_t temp = 0;

    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_REG_COEFF1, coeff, BME690_LEN_COEFF1),
                        TAG,
                        "读取 BME690 校准区 1 失败");
    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_REG_COEFF2, coeff + BME690_LEN_COEFF1, BME690_LEN_COEFF2),
                        TAG,
                        "读取 BME690 校准区 2 失败");

    s_bme690_calib.par_t2 = s16_from_bytes(coeff[1], coeff[2]);
    s_bme690_calib.par_t3 = (int8_t)coeff[3];
    s_bme690_calib.par_p1 = u16_from_bytes(coeff[5], coeff[6]);
    s_bme690_calib.par_p2 = s16_from_bytes(coeff[7], coeff[8]);
    s_bme690_calib.par_p3 = (int8_t)coeff[9];
    s_bme690_calib.par_p4 = s16_from_bytes(coeff[11], coeff[12]);
    s_bme690_calib.par_p5 = s16_from_bytes(coeff[13], coeff[14]);
    s_bme690_calib.par_p7 = (int8_t)coeff[15];
    s_bme690_calib.par_p6 = (int8_t)coeff[16];
    s_bme690_calib.par_p8 = s16_from_bytes(coeff[19], coeff[20]);
    s_bme690_calib.par_p9 = s16_from_bytes(coeff[21], coeff[22]);
    s_bme690_calib.par_p10 = coeff[23];

    s_bme690_calib.par_h2 = (uint16_t)(((uint16_t)coeff[25] << 4) | (coeff[26] >> 4));
    s_bme690_calib.par_h1 = (uint16_t)(((uint16_t)coeff[27] << 4) | (coeff[26] & 0x0F));
    s_bme690_calib.par_h3 = (int8_t)coeff[28];
    s_bme690_calib.par_h4 = (int8_t)coeff[29];
    s_bme690_calib.par_h5 = (int8_t)coeff[30];
    s_bme690_calib.par_h6 = coeff[31];
    s_bme690_calib.par_h7 = (int8_t)coeff[32];

    s_bme690_calib.par_t1 = u16_from_bytes(coeff[33], coeff[34]);
    s_bme690_calib.par_gh2 = s16_from_bytes(coeff[35], coeff[36]);
    s_bme690_calib.par_gh1 = (int8_t)coeff[37];
    s_bme690_calib.par_gh3 = (int8_t)coeff[38];

    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_REG_RES_HEAT_RANGE, &temp, 1),
                        TAG,
                        "读取 res_heat_range 失败");
    s_bme690_calib.res_heat_range = (temp & 0x30) / 16;

    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_REG_RES_HEAT_VAL, &temp, 1),
                        TAG,
                        "读取 res_heat_val 失败");
    s_bme690_calib.res_heat_val = (int8_t)temp;

    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_REG_RANGE_SW_ERR, &temp, 1),
                        TAG,
                        "读取 range_sw_err 失败");
    s_bme690_calib.range_sw_err = (int8_t)((temp & 0xF0) / 16);

    ESP_LOGI(TAG, "BME690 校准参数读取完成");
    return ESP_OK;
}

static esp_err_t bme690_configure(void)
{
    uint8_t chip_id = 0;

    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_CHIP_ID_REG, &chip_id, 1), TAG, "读取 BME690 Chip ID 失败");
    if (chip_id != BME690_CHIP_ID) {
        ESP_LOGE(TAG, "Chip ID 不匹配：读到 0x%02X，期望 0x%02X", chip_id, BME690_CHIP_ID);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ESP_LOGI(TAG, "检测到 BME690/BME680，I2C 地址：0x%02X，Chip ID：0x%02X", s_bme690_addr, chip_id);

    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_RESET_REG, BME690_SOFT_RESET_CMD), TAG, "BME690 软复位失败");
    vTaskDelay(pdMS_TO_TICKS(50));

    uint8_t variant_id = 0;
    esp_err_t variant_ret = bme690_read_regs(BME690_REG_VARIANT_ID, &variant_id, 1);
    if (variant_ret == ESP_OK) {
        s_bme690_variant_id = variant_id;
        ESP_LOGI(TAG,
                 "BME690 variant_id=0x%02X，气体电阻算法=%s",
                 s_bme690_variant_id,
                 (s_bme690_variant_id == BME690_VARIANT_GAS_HIGH) ? "high gas variant" : "low gas variant");
    } else {
        s_bme690_variant_id = BME690_VARIANT_GAS_LOW;
        ESP_LOGW(TAG,
                 "读取 BME690 variant_id 失败：%s，按 low gas variant 兼容模式解析气体电阻",
                 esp_err_to_name(variant_ret));
    }

    ESP_RETURN_ON_ERROR(bme690_read_calibration(), TAG, "读取 BME690 校准参数失败");

    /* 湿度过采样 x1 */
    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_REG_CTRL_HUM, 0x01), TAG, "配置湿度过采样失败");

    /* IIR 滤波系数 3 */
    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_REG_CONFIG, 0x0C), TAG, "配置滤波器失败");

    /* 加热器：约 320°C，约 150ms，用于气体电阻测量 */
    uint8_t heater_res = bme690_calc_heater_res(320, 25);
    uint8_t gas_wait = bme690_calc_gas_wait(150);
    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_REG_RES_HEAT_0, heater_res), TAG, "配置加热电阻失败");
    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_REG_GAS_WAIT_0, gas_wait), TAG, "配置加热等待时间失败");

    /* run_gas=1, nb_conv=0 */
    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_REG_CTRL_GAS_1, 0x10), TAG, "启用气体测量失败");

    ESP_LOGI(TAG, "BME690 配置完成：heater_res=%u, gas_wait=%u", heater_res, gas_wait);
    return ESP_OK;
}

static double bme690_compensate_temperature(uint32_t adc_temp)
{
    double var1 = (((double)adc_temp / 16384.0) - ((double)s_bme690_calib.par_t1 / 1024.0)) *
                  (double)s_bme690_calib.par_t2;
    double var2 = ((((double)adc_temp / 131072.0) - ((double)s_bme690_calib.par_t1 / 8192.0)) *
                   (((double)adc_temp / 131072.0) - ((double)s_bme690_calib.par_t1 / 8192.0))) *
                  ((double)s_bme690_calib.par_t3 * 16.0);

    s_bme690_calib.t_fine = var1 + var2;
    return s_bme690_calib.t_fine / 5120.0;
}

static double bme690_compensate_pressure(uint32_t adc_pres)
{
    /* Bosch BME680/BME690 官方整数补偿算法，返回 Pa。
     * 原 double 公式在部分 BME690 校准数据上容易放大误差，表现为 2000+hPa 的异常值。
     */
    int64_t var1 = (((int64_t)s_bme690_calib.t_fine) >> 1) - 64000;
    int64_t var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * (int64_t)s_bme690_calib.par_p6) >> 2;
    var2 = var2 + ((var1 * (int64_t)s_bme690_calib.par_p5) << 1);
    var2 = (var2 >> 2) + ((int64_t)s_bme690_calib.par_p4 << 16);

    var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * ((int64_t)s_bme690_calib.par_p3 << 5)) >> 3) +
           (((int64_t)s_bme690_calib.par_p2 * var1) >> 1);
    var1 = var1 >> 18;
    var1 = ((32768 + var1) * (int64_t)s_bme690_calib.par_p1) >> 15;

    if (var1 == 0) {
        return 0.0;
    }

    int64_t pressure = 1048576 - (int64_t)adc_pres;
    pressure = (pressure - (var2 >> 12)) * 3125;

    if (pressure >= (1LL << 30)) {
        pressure = (pressure / var1) << 1;
    } else {
        pressure = (pressure << 1) / var1;
    }

    var1 = ((int64_t)s_bme690_calib.par_p9 * (((pressure >> 3) * (pressure >> 3)) >> 13)) >> 12;
    var2 = ((pressure >> 2) * (int64_t)s_bme690_calib.par_p8) >> 13;
    int64_t var3 = ((pressure >> 8) * (pressure >> 8) * (pressure >> 8) *
                    (int64_t)s_bme690_calib.par_p10) >>
                   17;

    pressure = pressure + ((var1 + var2 + var3 + ((int64_t)s_bme690_calib.par_p7 << 7)) >> 4);
    return (double)pressure;
}

static double bme690_compensate_humidity(uint16_t adc_hum, double temp_comp)
{
    double var1 = (double)adc_hum -
                  (((double)s_bme690_calib.par_h1 * 16.0) +
                   (((double)s_bme690_calib.par_h3 / 2.0) * temp_comp));
    double var2 = var1 *
                  (((double)s_bme690_calib.par_h2 / 262144.0) *
                   (1.0 + (((double)s_bme690_calib.par_h4 / 16384.0) * temp_comp) +
                    (((double)s_bme690_calib.par_h5 / 1048576.0) * temp_comp * temp_comp)));
    double var3 = (double)s_bme690_calib.par_h6 / 16384.0;
    double var4 = (double)s_bme690_calib.par_h7 / 2097152.0;
    double humidity = var2 + ((var3 + (var4 * temp_comp)) * var2 * var2);

    if (humidity > 100.0) {
        humidity = 100.0;
    } else if (humidity < 0.0) {
        humidity = 0.0;
    }

    return humidity;
}

static double bme690_calc_gas_resistance_low(uint16_t adc_gas_res, uint8_t gas_range)
{
    static const uint32_t lookup_table1[16] = {
        2147483647U, 2147483647U, 2147483647U, 2147483647U,
        2147483647U, 2126008810U, 2147483647U, 2130303777U,
        2147483647U, 2147483647U, 2143188679U, 2136746228U,
        2147483647U, 2126008810U, 2147483647U, 2147483647U};
    static const uint32_t lookup_table2[16] = {
        4096000000U, 2048000000U, 1024000000U, 512000000U,
        255744255U, 127110228U, 64000000U, 32258064U,
        16016016U, 8000000U, 4000000U, 2000000U,
        1000000U, 500000U, 250000U, 125000U};

    if (gas_range > 15) {
        gas_range = 15;
    }

    int64_t var1 = ((int64_t)1340 + (5 * (int64_t)s_bme690_calib.range_sw_err)) *
                   (int64_t)lookup_table1[gas_range] /
                   65536;
    int64_t var2 = (((int64_t)adc_gas_res * 32768) - 16777216) + var1;
    int64_t var3 = ((int64_t)lookup_table2[gas_range] * var1) / 512;

    if (var2 == 0) {
        return 0.0;
    }

    return (double)((var3 + (var2 / 2)) / var2);
}

static double bme690_calc_gas_resistance_high(uint16_t adc_gas_res, uint8_t gas_range)
{
    if (gas_range > 15) {
        gas_range = 15;
    }

    uint32_t var1 = 262144U >> gas_range;
    int32_t var2 = ((int32_t)adc_gas_res - 512) * 3 + 4096;

    if (var2 <= 0) {
        return 0.0;
    }

    return (double)(((10000U * var1) / (uint32_t)var2) * 100U);
}

static esp_err_t bme690_read_values(bme690_values_t *values)
{
    uint8_t data[17] = {0};

    /* 触发 forced mode：温度 x2、气压 x4、forced mode */
    ESP_RETURN_ON_ERROR(bme690_write_reg(BME690_REG_CTRL_MEAS, (2 << 5) | (3 << 2) | 1),
                        TAG,
                        "触发 BME690 测量失败");

    vTaskDelay(pdMS_TO_TICKS(250));

    ESP_RETURN_ON_ERROR(bme690_read_regs(BME690_REG_FIELD0, data, sizeof(data)), TAG, "读取 BME690 数据失败");

    bool new_data = (data[0] & 0x80) != 0;
    if (!new_data) {
        ESP_LOGW(TAG, "BME690 本次没有 new_data 标志，仍尝试解析数据");
    }

    /* 不同 Bosch 驱动版本对 FIELD0 前导字节说明不完全一致。
     * 这里同时尝试 pressure 起始偏移 1/2/3，并选择气压最接近正常大气压范围的解析结果。
     * 这样可兼容 BME680/BME690 在不同资料中的 FIELD0 布局差异。
     */
    double temp_c = 0.0;
    double pressure_pa = 0.0;
    double humidity = 0.0;
    int selected_offset = 1;
    double best_score = DBL_MAX;

    for (int offset = 1; offset <= 3; offset++) {
        uint32_t cand_adc_pres = ((uint32_t)data[offset] << 12) |
                                 ((uint32_t)data[offset + 1] << 4) |
                                 ((uint32_t)data[offset + 2] >> 4);
        uint32_t cand_adc_temp = ((uint32_t)data[offset + 3] << 12) |
                                 ((uint32_t)data[offset + 4] << 4) |
                                 ((uint32_t)data[offset + 5] >> 4);
        uint16_t cand_adc_hum = ((uint16_t)data[offset + 6] << 8) | data[offset + 7];

        double cand_temp_c = bme690_compensate_temperature(cand_adc_temp);
        double cand_pressure_pa = bme690_compensate_pressure(cand_adc_pres);
        double cand_pressure_hpa = cand_pressure_pa / 100.0;
        double cand_humidity = bme690_compensate_humidity(cand_adc_hum, cand_temp_c);

        double score = fabs(cand_pressure_hpa - 1013.25);
        if (cand_pressure_hpa < 300.0 || cand_pressure_hpa > 1100.0) {
            score += 10000.0 + fabs(cand_pressure_hpa - 1013.25);
        }
        if (cand_temp_c < -40.0 || cand_temp_c > 85.0) {
            score += 10000.0 + fabs(cand_temp_c - 25.0);
        }
        if (cand_humidity < 0.0 || cand_humidity > 100.0) {
            score += 10000.0;
        }

        if (score < best_score) {
            best_score = score;
            selected_offset = offset;
            temp_c = cand_temp_c;
            pressure_pa = cand_pressure_pa;
            humidity = cand_humidity;
        }
    }

    static int s_last_selected_offset = 0;
    if (s_last_selected_offset != selected_offset) {
        s_last_selected_offset = selected_offset;
        ESP_LOGW(TAG,
                 "BME690 FIELD0 自动选择解析偏移=%d，raw=%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X",
                 selected_offset,
                 data[0],
                 data[1],
                 data[2],
                 data[3],
                 data[4],
                 data[5],
                 data[6],
                 data[7],
                 data[8],
                 data[9],
                 data[10]);
    }

    uint8_t gas_msb_index = (s_bme690_variant_id == BME690_VARIANT_GAS_HIGH) ? 15 : 13;
    uint8_t gas_lsb_index = gas_msb_index + 1;
    uint16_t adc_gas_res = ((uint16_t)data[gas_msb_index] << 2) | (data[gas_lsb_index] >> 6);
    uint8_t gas_range = data[gas_lsb_index] & 0x0F;

    values->gas_valid = (data[gas_lsb_index] & 0x20) != 0;
    values->heat_stable = (data[gas_lsb_index] & 0x10) != 0;

    double gas_ohm = (s_bme690_variant_id == BME690_VARIANT_GAS_HIGH)
                         ? bme690_calc_gas_resistance_high(adc_gas_res, gas_range)
                         : bme690_calc_gas_resistance_low(adc_gas_res, gas_range);

    values->temperature_c = (float)temp_c;
    values->pressure_hpa = (float)(pressure_pa / 100.0);
    values->humidity_percent = (float)humidity;
    values->gas_resistance_ohm = (float)gas_ohm;

    return ESP_OK;
}

static esp_err_t bme690_init(void)
{
    ESP_RETURN_ON_ERROR(i2c_bus_init(), TAG, "I2C 总线初始化失败");
    ESP_RETURN_ON_ERROR(bme690_probe_and_add(), TAG, "BME690 探测失败");
    ESP_RETURN_ON_ERROR(bme690_configure(), TAG, "BME690 配置失败");
    return ESP_OK;
}

/* ---------------- OneNET MQTT ---------------- */

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT 已连接 OneNET");
        xEventGroupSetBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);

        char reply_topic[160] = {0};
        snprintf(reply_topic,
                 sizeof(reply_topic),
                 "$sys/%s/%s/thing/property/post/reply",
                 CONFIG_ONENET_PRODUCT_ID,
                 CONFIG_ONENET_DEVICE_NAME);
        int msg_id = esp_mqtt_client_subscribe(s_mqtt_client, reply_topic, 1);
        ESP_LOGI(TAG, "订阅 OneNET 属性上报回复：topic=%s, msg_id=%d", reply_topic, msg_id);
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT 已断开");
        xEventGroupClearBits(s_mqtt_event_group, MQTT_CONNECTED_BIT);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG, "MQTT 发布完成，msg_id=%d", event->msg_id);
        break;

    case MQTT_EVENT_DATA:
        ESP_LOGI(TAG,
                 "收到 OneNET MQTT 回复：topic=%.*s, data=%.*s",
                 event->topic_len,
                 event->topic,
                 event->data_len,
                 event->data);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT 事件错误");
        break;

    default:
        break;
    }
}

static bool onenet_url_encode(const char *src, char *dst, size_t dst_len)
{
    size_t out = 0;

    for (size_t i = 0; src[i] != '\0'; i++) {
        unsigned char c = (unsigned char)src[i];
        bool unreserved = ((c >= 'A' && c <= 'Z') ||
                           (c >= 'a' && c <= 'z') ||
                           (c >= '0' && c <= '9') ||
                           c == '-' || c == '_' || c == '.' || c == '~');

        if (unreserved) {
            if (out + 1 >= dst_len) {
                return false;
            }
            dst[out++] = (char)c;
        } else {
            if (out + 3 >= dst_len) {
                return false;
            }
            static const char hex[] = "0123456789ABCDEF";
            dst[out++] = '%';
            dst[out++] = hex[(c >> 4) & 0x0F];
            dst[out++] = hex[c & 0x0F];
        }
    }

    if (out >= dst_len) {
        return false;
    }
    dst[out] = '\0';
    return true;
}

static esp_err_t onenet_build_mqtt_password(char *password, size_t password_len)
{
    const char *device_secret_or_token = CONFIG_ONENET_DEVICE_TOKEN;
    if (strncmp(device_secret_or_token, "version=", strlen("version=")) == 0) {
        strlcpy(password, device_secret_or_token, password_len);
        ESP_LOGI(TAG, "检测到 ONENET_DEVICE_TOKEN 已是完整 token，直接作为 MQTT password 使用");
        return ESP_OK;
    }

    char res[160] = {0};
    char sign_content[256] = {0};
    char encoded_res[224] = {0};
    char encoded_sign[128] = {0};
    unsigned char key[128] = {0};
    size_t key_len = 0;
    unsigned char hmac[20] = {0};
    unsigned char sign_base64[64] = {0};
    size_t sign_base64_len = 0;

    int written = snprintf(res,
                           sizeof(res),
                           "products/%s/devices/%s",
                           CONFIG_ONENET_PRODUCT_ID,
                           CONFIG_ONENET_DEVICE_NAME);
    if (written <= 0 || written >= (int)sizeof(res)) {
        ESP_LOGE(TAG, "OneNET res 生成失败，product_id/device_name 过长");
        return ESP_ERR_INVALID_SIZE;
    }

    long long expire_time = (long long)CONFIG_ONENET_TOKEN_EXPIRE_TIMESTAMP;
    written = snprintf(sign_content,
                       sizeof(sign_content),
                       "%lld\n%s\n%s\n%s",
                       expire_time,
                       ONENET_TOKEN_METHOD,
                       res,
                       ONENET_TOKEN_VERSION);
    if (written <= 0 || written >= (int)sizeof(sign_content)) {
        ESP_LOGE(TAG, "OneNET 签名原文生成失败");
        return ESP_ERR_INVALID_SIZE;
    }

    int b64_ret = mbedtls_base64_decode(key,
                                        sizeof(key),
                                        &key_len,
                                        (const unsigned char *)device_secret_or_token,
                                        strlen(device_secret_or_token));
    if (b64_ret != 0 || key_len == 0) {
        key_len = strlen(device_secret_or_token);
        if (key_len >= sizeof(key)) {
            ESP_LOGE(TAG, "OneNET 设备密钥过长");
            return ESP_ERR_INVALID_SIZE;
        }
        memcpy(key, device_secret_or_token, key_len);
        ESP_LOGW(TAG, "OneNET 设备密钥 Base64 解码失败，改用原始字符串作为 HMAC key");
    }

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA1);
    if (md_info == NULL) {
        ESP_LOGE(TAG, "mbedTLS 不支持 SHA1 HMAC");
        return ESP_FAIL;
    }

    if (mbedtls_md_hmac(md_info,
                        key,
                        key_len,
                        (const unsigned char *)sign_content,
                        strlen(sign_content),
                        hmac) != 0) {
        ESP_LOGE(TAG, "OneNET HMAC-SHA1 计算失败");
        return ESP_FAIL;
    }

    memset(key, 0, sizeof(key));

    if (mbedtls_base64_encode(sign_base64,
                              sizeof(sign_base64),
                              &sign_base64_len,
                              hmac,
                              sizeof(hmac)) != 0) {
        ESP_LOGE(TAG, "OneNET HMAC 结果 Base64 编码失败");
        return ESP_FAIL;
    }
    sign_base64[sign_base64_len] = '\0';

    if (!onenet_url_encode(res, encoded_res, sizeof(encoded_res)) ||
        !onenet_url_encode((const char *)sign_base64, encoded_sign, sizeof(encoded_sign))) {
        ESP_LOGE(TAG, "OneNET token URL 编码失败");
        return ESP_ERR_INVALID_SIZE;
    }

    written = snprintf(password,
                       password_len,
                       "version=%s&res=%s&et=%lld&method=%s&sign=%s",
                       ONENET_TOKEN_VERSION,
                       encoded_res,
                       expire_time,
                       ONENET_TOKEN_METHOD,
                       encoded_sign);
    if (written <= 0 || written >= (int)password_len) {
        ESP_LOGE(TAG, "OneNET token 缓冲区不足");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "已根据设备密钥生成 OneNET MQTT token，res=%s, et=%lld, method=%s",
             res,
             expire_time,
             ONENET_TOKEN_METHOD);
    return ESP_OK;
}

static void mqtt_app_start(void)
{
    s_mqtt_event_group = xEventGroupCreate();

    esp_err_t token_ret = onenet_build_mqtt_password(s_onenet_mqtt_password, sizeof(s_onenet_mqtt_password));
    if (token_ret != ESP_OK) {
        ESP_LOGE(TAG, "生成 OneNET MQTT password/token 失败：%s", esp_err_to_name(token_ret));
        return;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = CONFIG_ONENET_MQTT_URI,
        .credentials.client_id = CONFIG_ONENET_DEVICE_NAME,
        .credentials.username = CONFIG_ONENET_PRODUCT_ID,
        .credentials.authentication.password = s_onenet_mqtt_password,
    };

    ESP_LOGI(TAG, "启动 MQTT：uri=%s, product_id=%s, device_name=%s",
             CONFIG_ONENET_MQTT_URI,
             CONFIG_ONENET_PRODUCT_ID,
             CONFIG_ONENET_DEVICE_NAME);

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
}

static float clamp_property_value(const char *name, float value, float min_value, float max_value)
{
    if (value < min_value) {
        ESP_LOGW(TAG, "%s=%.2f 小于物模型最小值 %.2f，将按最小值上报", name, value, min_value);
        return min_value;
    }

    if (value > max_value) {
        ESP_LOGW(TAG, "%s=%.2f 大于物模型最大值 %.2f，将按最大值上报", name, value, max_value);
        return max_value;
    }

    return value;
}

static float normalize_property_value(const char *name, float value, float min_value, float max_value, float step)
{
    value = clamp_property_value(name, value, min_value, max_value);
    if (step > 0.0f) {
        value = roundf(value / step) * step;
    }
    return clamp_property_value(name, value, min_value, max_value);
}

static void onenet_publish_property(const bme690_values_t *values)
{
    if (s_mqtt_client == NULL || s_mqtt_event_group == NULL) {
        return;
    }

    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if ((bits & MQTT_CONNECTED_BIT) == 0) {
        ESP_LOGW(TAG, "MQTT 未连接，本次跳过上报");
        return;
    }

    char topic[160] = {0};
    char payload[512] = {0};

    snprintf(topic,
             sizeof(topic),
             "$sys/%s/%s/thing/property/post",
             CONFIG_ONENET_PRODUCT_ID,
             CONFIG_ONENET_DEVICE_NAME);

    int msg_id_num = (int)(xTaskGetTickCount() & 0x7FFFFFFF);

    float temperature = normalize_property_value("temperature",
                                                 values->temperature_c,
                                                 ONENET_MODEL_TEMPERATURE_MIN,
                                                 ONENET_MODEL_TEMPERATURE_MAX,
                                                 0.1f);
    float humidity = normalize_property_value("humidity",
                                              values->humidity_percent,
                                              ONENET_MODEL_HUMIDITY_MIN,
                                              ONENET_MODEL_HUMIDITY_MAX,
                                              0.1f);
    float pressure = normalize_property_value("pressure",
                                              values->pressure_hpa,
                                              ONENET_MODEL_PRESSURE_MIN,
                                              ONENET_MODEL_PRESSURE_MAX,
                                              0.1f);
    bool include_gas_resistance = values->gas_valid &&
                                  values->heat_stable &&
                                  isfinite(values->gas_resistance_ohm) &&
                                  values->gas_resistance_ohm >= ONENET_MODEL_GAS_RESISTANCE_MIN &&
                                  values->gas_resistance_ohm <= ONENET_MODEL_GAS_RESISTANCE_MAX;

    if (!include_gas_resistance) {
        ESP_LOGW(TAG,
                 "气体电阻无效或超出物模型范围，本次不上报 gas_resistance：value=%.2fΩ, gas_valid=%d, heat_stable=%d",
                 values->gas_resistance_ohm,
                 values->gas_valid,
                 values->heat_stable);
    }

    /* OneNET 物模型属性上报 JSON。
     * 注意：temperature/humidity/pressure/gas_resistance 需要与你在 OneNET 产品物模型中定义的标识符一致。
     * 气体电阻只有在 BME690 标记有效且数值在物模型范围内时才上报，避免用钳制后的假数据污染平台数据。
     */
    if (include_gas_resistance) {
        float gas_resistance = normalize_property_value("gas_resistance",
                                                        values->gas_resistance_ohm,
                                                        ONENET_MODEL_GAS_RESISTANCE_MIN,
                                                        ONENET_MODEL_GAS_RESISTANCE_MAX,
                                                        1.0f);

        snprintf(payload,
                 sizeof(payload),
                 "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{"
                 "\"temperature\":{\"value\":%.1f},"
                 "\"humidity\":{\"value\":%.1f},"
                 "\"pressure\":{\"value\":%.1f},"
                 "\"gas_resistance\":{\"value\":%.0f}"
                 "}}",
                 msg_id_num,
                 temperature,
                 humidity,
                 pressure,
                 gas_resistance);
    } else {
        snprintf(payload,
                 sizeof(payload),
                 "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{"
                 "\"temperature\":{\"value\":%.1f},"
                 "\"humidity\":{\"value\":%.1f},"
                 "\"pressure\":{\"value\":%.1f}"
                 "}}",
                 msg_id_num,
                 temperature,
                 humidity,
                 pressure);
    }

    int msg_id = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "上报 OneNET：topic=%s, msg_id=%d, payload=%s", topic, msg_id, payload);
}

/* ---------------- SNTP 时间同步 ---------------- */

/* 火山引擎 WebSocket 与 DynamicRegister 都用 Unix 毫秒时间戳，并校验时间窗。
 * 必须在 Wi-Fi 连接后做 SNTP 同步，否则签名一定 401。
 */
static bool sntp_time_sync(void)
{
    ESP_LOGI(TAG, "开始 SNTP 时间同步...");
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    cfg.sync_cb = NULL;
    esp_netif_sntp_init(&cfg);

    setenv("TZ", "CST-8", 1);
    tzset();

    int retry = 0;
    const int retry_max = 20;
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) == ESP_ERR_TIMEOUT && retry < retry_max) {
        retry++;
        ESP_LOGI(TAG, "等待 SNTP 同步... (%d/%d)", retry, retry_max);
    }

    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        ESP_LOGE(TAG, "SNTP 同步失败，now=%lld", (long long)now);
        esp_netif_sntp_deinit();
        return false;
    }

    struct tm ti;
    localtime_r(&now, &ti);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
    ESP_LOGI(TAG, "SNTP 时间同步成功：%s (epoch=%lld)", buf, (long long)now);
    return true;
}

/* ---------------- 主任务 ---------------- */

static void bme690_onenet_task(void *arg)
{
    esp_err_t ret = bme690_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BME690 初始化失败：%s", esp_err_to_name(ret));
        vTaskDelete(NULL);
        return;
    }

    if (s_wifi_connected) {
        ESP_LOGI(TAG, "Wi-Fi 已连接，启动 OneNET MQTT 上报");
        mqtt_app_start();
    } else {
        ESP_LOGW(TAG, "Wi-Fi 未连接，BME690 将只在串口输出数据，暂不启动 OneNET MQTT");
    }

    while (1) {
        bme690_values_t values = {0};
        ret = bme690_read_values(&values);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG,
                     "BME690 数据：温度=%.2f°C, 湿度=%.2f%%, 气压=%.2fhPa, 气体电阻=%.2fΩ, gas_valid=%d, heat_stable=%d",
                     values.temperature_c,
                     values.humidity_percent,
                     values.pressure_hpa,
                     values.gas_resistance_ohm,
                     values.gas_valid,
                     values.heat_stable);

            onenet_publish_property(&values);
        } else {
            ESP_LOGE(TAG, "读取 BME690 失败：%s", esp_err_to_name(ret));
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_ONENET_PROPERTY_POST_INTERVAL_SECONDS * 1000));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    if (CONFIG_LOG_MAXIMUM_LEVEL > CONFIG_LOG_DEFAULT_LEVEL) {
        esp_log_level_set("wifi", CONFIG_LOG_MAXIMUM_LEVEL);
    }

    ESP_LOGI(TAG, "ESP32-C5 Wi-Fi + BME690 + OneNET MQTT 示例启动");

    bool wifi_connected = wifi_init_sta();
    if (!wifi_connected) {
        ESP_LOGW(TAG, "Wi-Fi 未连接，仍继续启动 BME690 传感器串口显示，OneNET 上报会被跳过");
    }

    /* Wi-Fi 已连接后启动火山引擎硬件对话智能体（WebSocket 协议）。
     * 在 menuconfig -> Component config -> 火山引擎对话智能体（WebSocket）
     * 中填好 InstanceID / product_key / product_secret / bot_id 等参数。
     *
     * 注意：必须先 SNTP 同步真实时间，否则签名时间戳错误，服务端会直接返回 401。
     */
    if (wifi_connected) {
        if (!sntp_time_sync()) {
            ESP_LOGE(TAG, "SNTP 同步失败，无法启动火山引擎（签名依赖系统时间）");
        } else {
            if (volc_conv_app_start()) {
                ESP_LOGI(TAG, "火山引擎对话智能体启动流程已发起");
            } else {
                ESP_LOGE(TAG, "火山引擎对话智能体启动失败");
            }
        }
    } else {
        ESP_LOGW(TAG, "Wi-Fi 未连接，跳过火山引擎对话智能体启动");
    }

    xTaskCreate(bme690_onenet_task, "bme690_onenet_task", 8192, NULL, 5, NULL);
}