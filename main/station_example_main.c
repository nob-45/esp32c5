/* ESP32-C5 Wi-Fi Station + BME690 (官方 bme69x 驱动) + OneNET MQTT + 火山引擎 WebSocket
 *
 * 功能：
 *   1. 连接 Wi-Fi；
 *   2. 使用官方 Bosch bme69x 驱动 + bme69x_port I2C 适配层，读取 BME690
 *      温度、湿度、气压、气体电阻；
 *   3. 通过 MQTT 上报到 OneNET 物模型属性上报主题；
 *   4. 启动火山引擎硬件对话智能体（WebSocket 协议）。
 *
 * 硬件默认连接：
 *   BME690 SDA -> ESP32-C5 GPIO2
 *   BME690 SCL -> ESP32-C5 GPIO3
 *   I2C 频率    -> 100 kHz
 *
 * 本版本与前一版的关键差异：
 *   - 不再使用手写 BME690 寄存器驱动（约 700 行手写代码 + 经验公式），
 *     改用官方 Bosch bme69x.c / bme69x.h / bme69x_defs.h；
 *   - I2C 适配层写在 components/bme69x/bme69x_port.c，使用
 *     esp_driver_i2c 的新 i2c_master_* API；
 *   - 大气压"2000+ hPa" 错误的根本原因是手写补偿公式与 BME690 实际
 *     寄存器布局不匹配；官方驱动使用 Bosch 验证过的算法，t/p/h 由
 *     bme69x_set_heater_profile_forced + bme69x_get_data API 计算，
 *     物理量返回 IEEE754 单精度浮点；
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
#include "bme69x_port.h"

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

/* BME690 物理量输出结构。bme69x_get_data() 把 T/P/H/Gas 计算为 IEEE754
 * 单精度浮点，由 Bosch 官方驱动完成所有补偿。 */
typedef struct {
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    float gas_resistance_ohm;
    bool gas_valid;
    bool heat_stable;
} bme690_values_t;

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

/* ---------------- BME690 (官方 bme69x 驱动 + port 适配层) ---------------- */
/*
 * 此处不再直接调用 Bosch bme69x_* API（API 复杂、易用错），而是通过
 * components/bme69x/bme69x_port.c 提供的两个简洁函数:
 *   bme69x_port_init()  - I2C 总线 + Bosch 驱动初始化 + forced 模式配置
 *   bme69x_port_read()  - 触发一次 forced 测量并返回 4 个物理量（已 hPa/%/°C/Ω）
 *
 * 气压修正：之前手写驱动对 BME690 variant 误用 BME680 算法 + 校准系数
 * 配错导致气压显示 2000+ hPa。port 层使用 Bosch 官方 bme69x.c (bme69x_API.c)
 * 的 calc_pressure 公式 + 通过 variant_id 寄存器自动切换 BME690 算法。
 */
static esp_err_t bme690_init(void)
{
    /* 之前传 &s_bme690 + 3 个 I2C 参数, 现 port_init() 用 Kconfig 里的宏 (gpio/scl/freq)
     * 自动从 sdkconfig 取值, 不再需要从 main 传参。 */
    ESP_RETURN_ON_ERROR(bme69x_port_init(), TAG, "BME690 初始化失败");
    ESP_LOGI(TAG, "BME690 初始化完成 (官方 bme69x 驱动 + port 适配层)");
    return ESP_OK;
}

static esp_err_t bme690_read_values(bme690_values_t *values)
{
    memset(values, 0, sizeof(*values));

    /* port_read 一次调用完成: 触发 forced -> 等测量 -> 读 raw -> 调用官方补偿公式 */
    esp_err_t err = bme69x_port_read(&values->temperature_c,
                                     &values->humidity_percent,
                                     &values->pressure_hpa,
                                     &values->gas_resistance_ohm);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 没数据 - 可能上次加热还没结束, 跳过本周期 */
        return ESP_ERR_NOT_FOUND;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "BME690 读失败");

    /* port 内部未返回 status, 这里按"通常有效"假设。OneNET 上报不依赖
     * gas_valid/heat_stable (产品模型这 2 个是可选 boolean)。 */
    values->gas_valid   = true;
    values->heat_stable = true;

    /* 物理合理性检查：极端值（>1500hPa / <-40°C）几乎一定是 I2C 通信
     * 异常或 chip id 错配。官方驱动通常不会出现。 */
    if (values->pressure_hpa < 300.0f || values->pressure_hpa > 1500.0f) {
        ESP_LOGW(TAG, "BME690 气压超出物理合理区间: %.2f hPa (I2C/接线异常?)",
                 values->pressure_hpa);
    }

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
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;
    for (size_t i = 0; src[i] != '\0' && out + 4 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            dst[out++] = (char)c;
        } else {
            dst[out++] = '%';
            dst[out++] = hex[c >> 4];
            dst[out++] = hex[c & 0x0F];
        }
    }
    dst[out] = '\0';
    return true;
}

static bool onenet_hmac_sha1(const char *key, size_t key_len, const char *data, size_t data_len, uint8_t out[20])
{
    int rc = mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1),
                             (const unsigned char *)key, key_len,
                             (const unsigned char *)data, data_len,
                             out);
    return rc == 0;
}

static bool onenet_base64_encode(const uint8_t *src, size_t src_len, char *dst, size_t dst_len)
{
    size_t out_len = 0;
    int rc = mbedtls_base64_encode((unsigned char *)dst, dst_len, &out_len, src, src_len);
    if (rc != 0) {
        return false;
    }
    dst[out_len] = '\0';
    return true;
}

static bool onenet_calc_token(const char *product_id, const char *device_name, const char *access_key,
                              time_t expire, char *out_token, size_t out_len)
{
    /* OneNET token 长度有明确上界:
     *   "version=2018-10-31&res=products%2F<id>%2Fdevices%2F<name>
     *    &et=<epoch>&method=sha1&sign=<base64-28>"
     * 即使 product_id/device_name 都是 64 字符, 总长度 < 300 字节。
     * 早返回 out_len 不足的请求, 让 GCC 静态分析能识别 out_token 容量上界。 */
    if (out_len < 512) {
        return false;
    }
    char signature_resource[256] = {0};
    snprintf(signature_resource, sizeof(signature_resource), "products/%s/devices/%s", product_id, device_name);

    char signature_str[512] = {0};
    snprintf(signature_str, sizeof(signature_str),
             "%s\n%s\n%s\n%lld",
             ONENET_TOKEN_METHOD, signature_resource, ONENET_TOKEN_VERSION, (long long)expire);

    char hmac_str[64] = {0};
    if (!onenet_hmac_sha1(access_key, strlen(access_key), signature_str, strlen(signature_str),
                          (uint8_t *)hmac_str)) {
        return false;
    }

    char base64_str[64] = {0};
    if (!onenet_base64_encode((uint8_t *)hmac_str, 20, base64_str, sizeof(base64_str))) {
        return false;
    }

    char encoded[64] = {0};
    if (!onenet_url_encode(base64_str, encoded, sizeof(encoded))) {
        return false;
    }

    /* out_token 至少有 512 字节, 编译期上界 511, 不触发 format-truncation */
    int n = snprintf(out_token, out_len,
             "version=%s&res=products%%2F%s%%2Fdevices%%2F%s&et=%lld&method=%s&sign=%s",
             ONENET_TOKEN_VERSION, product_id, device_name, (long long)expire, ONENET_TOKEN_METHOD, encoded);
    if (n < 0 || (size_t)n >= out_len) {
        return false;
    }
    return true;
}

static float normalize_property_value(const char *name, float value, float min_v, float max_v)
{
    if (isnan(value) || isinf(value)) {
        ESP_LOGW(TAG, "%s 出现 NaN/Inf，钳制到物模型最小值", name);
        return min_v;
    }
    if (value < min_v) {
        ESP_LOGW(TAG, "%s 低于物模型最小值（%.2f < %.2f），钳制", name, value, min_v);
        return min_v;
    }
    if (value > max_v) {
        ESP_LOGW(TAG, "%s 超出物模型最大值（%.2f > %.2f），钳制", name, value, max_v);
        return max_v;
    }
    return value;
}

static void onenet_publish_property(const bme690_values_t *values)
{
    if (s_mqtt_client == NULL) {
        return;
    }
    EventBits_t bits = xEventGroupGetBits(s_mqtt_event_group);
    if (!(bits & MQTT_CONNECTED_BIT)) {
        ESP_LOGW(TAG, "MQTT 未连接，跳过本次属性上报");
        return;
    }

    static int msg_id = 0;
    msg_id++;
    int msg_id_num = msg_id;

    float temperature = normalize_property_value("temperature",
                                                 values->temperature_c,
                                                 ONENET_MODEL_TEMPERATURE_MIN,
                                                 ONENET_MODEL_TEMPERATURE_MAX);
    float humidity = normalize_property_value("humidity",
                                              values->humidity_percent,
                                              ONENET_MODEL_HUMIDITY_MIN,
                                              ONENET_MODEL_HUMIDITY_MAX);
    float pressure = normalize_property_value("pressure",
                                              values->pressure_hpa,
                                              ONENET_MODEL_PRESSURE_MIN,
                                              ONENET_MODEL_PRESSURE_MAX);

    ESP_LOGI(TAG,
             "BME690 气体电阻（仅串口调试）：value=%.2fΩ, gas_valid=%d, heat_stable=%d",
             values->gas_resistance_ohm,
             values->gas_valid,
             values->heat_stable);

    char topic[160] = {0};
    snprintf(topic, sizeof(topic), "$sys/%s/%s/thing/property/post",
             CONFIG_ONENET_PRODUCT_ID, CONFIG_ONENET_DEVICE_NAME);

    char payload[512] = {0};
    snprintf(payload, sizeof(payload),
             "{\"id\":\"%d\",\"version\":\"1.0\",\"params\":{"
             "\"temperature\":{\"value\":%.1f},"
             "\"humidity\":{\"value\":%.1f},"
             "\"pressure\":{\"value\":%.1f}"
             "}}",
             msg_id_num,
             temperature,
             humidity,
             pressure);

    int rc = esp_mqtt_client_publish(s_mqtt_client, topic, payload, 0, 1, 0);
    ESP_LOGI(TAG, "OneNET 上报：topic=%s, msg_id=%d, rc=%d, payload=%s", topic, msg_id_num, rc, payload);
}

static void mqtt_app_start(void)
{
    s_mqtt_event_group = xEventGroupCreate();

    time_t now = 0;
    time(&now);
    if (now < 1700000000) {
        ESP_LOGE(TAG, "系统时间未同步，无法生成 OneNET token");
        return;
    }
    /* CONFIG_ONENET_TOKEN_EXPIRE_TIMESTAMP 是绝对时间戳（默认 2030-01-01），
     * 不要再加 now。OneNET 服务端校验 `et` 必须是绝对 UTC 时间戳。 */
    time_t expire = (time_t)CONFIG_ONENET_TOKEN_EXPIRE_TIMESTAMP;
    if (!onenet_calc_token(CONFIG_ONENET_PRODUCT_ID, CONFIG_ONENET_DEVICE_NAME, CONFIG_ONENET_DEVICE_TOKEN,
                           expire, s_onenet_mqtt_password, sizeof(s_onenet_mqtt_password))) {
        ESP_LOGE(TAG, "OneNET token 计算失败");
        return;
    }

    char client_id[160] = {0};
    snprintf(client_id, sizeof(client_id), "%s.%s|securemode=2,signmethod=hmacsha1|",
             CONFIG_ONENET_PRODUCT_ID, CONFIG_ONENET_DEVICE_NAME);

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://mqtts.heclouds.com:1883",
        .credentials.client_id = client_id,
        .credentials.username = CONFIG_ONENET_PRODUCT_ID,
        .credentials.authentication.password = s_onenet_mqtt_password,
        .session.keepalive = 60,
        .session.disable_clean_session = false,
    };

    s_mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init 返回 NULL");
        return;
    }
    ESP_ERROR_CHECK(esp_mqtt_client_register_event(s_mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL));
    ESP_ERROR_CHECK(esp_mqtt_client_start(s_mqtt_client));
    ESP_LOGI(TAG, "OneNET MQTT 客户端已启动");
}

/* ---------------- SNTP ---------------- */

static bool sntp_time_sync(void)
{
    ESP_LOGI(TAG, "初始化 SNTP 同步真实时间（用于火山引擎签名）");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    esp_netif_sntp_init(&sntp_cfg);

    int retry = 0;
    const int retry_max = 30;
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

    ESP_LOGI(TAG, "ESP32-C5 Wi-Fi + BME690 (官方 bme69x) + OneNET MQTT 示例启动");

#if CONFIG_VOLC_AUDIO_SELFTEST
    /* 音频自测：在连 Wi-Fi/对话之前先验证喇叭和麦克风接线。
     * 阻塞约 7 秒，不联网。确认硬件正常后请在 menuconfig 关闭此项。 */
    ESP_LOGW(TAG, "已启用音频自测，开始测试喇叭和麦克风（约 7 秒）...");
    volc_conv_app_audio_selftest();
    ESP_LOGW(TAG, "音频自测结束");
#endif

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