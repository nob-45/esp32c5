/* WebSocket 客户端 - 火山引擎硬件对话智能体
 *
 * wss://ai-gateway.vei.volces.com/v1/realtime?bot=<BOT_ID>
 * 鉴权信息放在 HTTP Header 中，X-Signature = base64(HMAC-SHA256(secret, canon))。
 *
 * 重连：服务端对 X-Timestamp 有 1 分钟时间窗校验，esp_websocket_client 内置
 * 自动重连会复用旧时间戳 Header，约 60s 后握手永久 400。故禁用内置重连，由
 * 监控任务在断线后用新时间戳重算签名并重建客户端。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_mac.h"
#include "esp_random.h"

#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "cJSON.h"

#include "volc_conv_internal.h"

static const char *TAG = "volc_ws";

#define VOLC_CONV_WS_URL  "wss://" VOLC_CONV_WS_HOST VOLC_CONV_WS_PATH "?bot=" CONFIG_VOLC_CONV_BOT_ID

static esp_websocket_client_handle_t s_ws = NULL;
static volc_conv_ws_audio_cb_t       s_audio_cb = NULL;
static void                         *s_audio_user = NULL;
static volatile bool                 s_connected = false;

static volc_conv_credentials_t s_cred;             /* 凭证副本，重连复用 */
static volatile bool           s_ws_run = false;        /* 期望保持连接 */
static volatile bool           s_need_reconnect = false;/* 断线待重连 */
static TaskHandle_t            s_supervisor = NULL;

/* 鉴权 Header 缓冲：client 在 connect 时引用该指针，需在连接期间保持有效。
 * supervisor 单线程串行重建，故静态安全。 */
static char s_headers[1024];

/* 上行 base64+JSON 复用缓冲 */
static char s_tx_buf[8192];

/* ================== 工具函数 ================== */

/* HMAC-SHA256(key,data) → base64，写入 out（需 ≥ 48 字节） */
static esp_err_t hmac_sha256_base64(const char *key, const char *data, char *out, size_t out_sz)
{
    unsigned char mac[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return ESP_FAIL;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    int rc = mbedtls_md_setup(&ctx, info, 1);
    if (rc == 0) rc = mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    if (rc == 0) rc = mbedtls_md_hmac_update(&ctx, (const unsigned char *)data, strlen(data));
    if (rc == 0) rc = mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);
    if (rc != 0) return ESP_FAIL;

    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_sz, &olen, mac, sizeof(mac)) != 0) {
        return ESP_FAIL;
    }
    out[olen] = '\0';
    return ESP_OK;
}

/* 生成本次连接的鉴权 Header（新时间戳/新随机数/新签名），写入 s_headers。
 * 每次（重）连接前调用，确保时间戳在服务端 1 分钟窗口内。 */
static esp_err_t build_auth_headers(const volc_conv_credentials_t *cred)
{
    /* MAC 作为 Hardware-Id */
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_str[18];
    snprintf(mac_str, sizeof(mac_str), "%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 时间戳用毫秒：与 DynamicRegister 注册成功时一致（平台统一用毫秒） */
    uint64_t ts_ms = (uint64_t)time(NULL) * 1000ULL;
    uint32_t rnd = esp_random() & 0x7fffffff;
    char random_num[16];
    snprintf(random_num, sizeof(random_num), "%lu", (unsigned long)rnd);

    /* 规范串：字段顺序与 DynamicRegister 一致
     * auth_type&device_name&random_num&product_key&timestamp（不含 instance_id），
     * 仅把签名密钥从 product_secret 换成 device_secret。 */
    char canon[512];
    snprintf(canon, sizeof(canon),
             "auth_type=1&device_name=%s&random_num=%s&product_key=%s&timestamp=%llu",
             cred->device_name,
             random_num,
             CONFIG_VOLC_CONV_PRODUCT_KEY,
             (unsigned long long)ts_ms);

    char sig[64];
    esp_err_t err = hmac_sha256_base64(cred->device_secret, canon, sig, sizeof(sig));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "签名计算失败");
        return err;
    }

    int n = snprintf(s_headers, sizeof(s_headers),
                     "X-Auth-Type: 1\r\n"
                     "X-Product-Key: %s\r\n"
                     "X-Device-Name: %s\r\n"
                     "X-Random-Num: %s\r\n"
                     "X-Timestamp: %llu\r\n"
                     "X-Instance-Id: %s\r\n"
                     "X-Hardware-Id: %s\r\n"
                     "X-Signature: %s\r\n",
                     CONFIG_VOLC_CONV_PRODUCT_KEY,
                     cred->device_name,
                     random_num,
                     (unsigned long long)ts_ms,
                     CONFIG_VOLC_CONV_INSTANCE_ID,
                     mac_str,
                     sig);
    if (n <= 0 || n >= (int)sizeof(s_headers)) {
        ESP_LOGE(TAG, "Header 缓冲不足");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "鉴权 Header 就绪 ts_ms=%llu device=%s",
             (unsigned long long)ts_ms, cred->device_name);
    return ESP_OK;
}

/* ================== 下行处理 ================== */

/* 解析服务端文本帧。关心 response.audio.delta（base64 PCM16）。 */
static void handle_server_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) return;

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        if (strcmp(type->valuestring, "response.audio.delta") == 0) {
            const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
            if (cJSON_IsString(delta) && delta->valuestring) {
                size_t b64_len = strlen(delta->valuestring);
                size_t pcm_cap = (b64_len / 4) * 3 + 4;
                uint8_t *pcm = malloc(pcm_cap);
                if (pcm) {
                    size_t olen = 0;
                    if (mbedtls_base64_decode(pcm, pcm_cap, &olen,
                                              (const unsigned char *)delta->valuestring,
                                              b64_len) == 0 && olen > 0) {
                        if (s_audio_cb) s_audio_cb(pcm, olen, s_audio_user);
                    }
                    free(pcm);
                }
            }
        } else if (strcmp(type->valuestring, "error") == 0) {
            char *s = cJSON_PrintUnformatted(root);
            ESP_LOGW(TAG, "服务端 error: %s", s ? s : "(null)");
            if (s) free(s);
        }
    }
    cJSON_Delete(root);
}

/* ================== WebSocket 事件 ================== */

static void ws_event_handler(void *handler_args, esp_event_base_t base,
                             int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *d = (esp_websocket_event_data_t *)event_data;
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "WebSocket 已连接");
        s_connected = true;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        s_connected = false;
        if (s_ws_run) s_need_reconnect = true;  /* 交给 supervisor 重建 */
        break;

    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x08) {            /* close frame */
            ESP_LOGW(TAG, "收到 close 帧");
            s_connected = false;
            if (s_ws_run) s_need_reconnect = true;
        } else if (d->op_code == 0x01 && d->data_len > 0) {  /* text */
            handle_server_text((const char *)d->data_ptr, d->data_len);
        }
        break;

    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "WebSocket 错误");
        if (s_ws_run) s_need_reconnect = true;
        break;

    default:
        break;
    }
}

/* ================== 客户端创建 / 销毁 ================== */

static void ws_client_destroy(void)
{
    if (s_ws) {
        esp_websocket_client_close(s_ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_connected = false;
}

/* 用最新鉴权 Header 创建并启动客户端 */
static esp_err_t ws_client_create_start(void)
{
    esp_err_t err = build_auth_headers(&s_cred);
    if (err != ESP_OK) return err;

    esp_websocket_client_config_t cfg = {
        .uri                       = VOLC_CONV_WS_URL,
        .headers                   = s_headers,
        .crt_bundle_attach         = esp_crt_bundle_attach,
        .disable_auto_reconnect    = true,    /* 关键：禁用内置重连，避免旧时间戳 */
        .reconnect_timeout_ms      = 5000,
        .network_timeout_ms        = 10000,
        .buffer_size               = 8192,
        .task_stack                = 6144,
        .task_prio                 = 5,
    };

    s_ws = esp_websocket_client_init(&cfg);
    if (!s_ws) {
        ESP_LOGE(TAG, "客户端 init 失败");
        return ESP_FAIL;
    }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event_handler, NULL);

    err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "客户端 start 失败: %s", esp_err_to_name(err));
        ws_client_destroy();
        return err;
    }
    return ESP_OK;
}

/* ================== 监控任务：断线后用新时间戳重建 ================== */

static void supervisor_task(void *arg)
{
    /* 首次连接 */
    if (ws_client_create_start() != ESP_OK) {
        ESP_LOGE(TAG, "首次连接启动失败");
    }

    while (s_ws_run) {
        if (s_need_reconnect) {
            s_need_reconnect = false;
            ESP_LOGW(TAG, "检测到断线，重建客户端（刷新时间戳/签名）");
            ws_client_destroy();
            /* 退避，避免握手风暴 */
            for (int i = 0; i < 30 && s_ws_run; ++i) vTaskDelay(pdMS_TO_TICKS(100));
            if (!s_ws_run) break;
            if (ws_client_create_start() != ESP_OK) {
                ESP_LOGE(TAG, "重建失败，5s 后再试");
                for (int i = 0; i < 50 && s_ws_run; ++i) vTaskDelay(pdMS_TO_TICKS(100));
                s_need_reconnect = true;  /* 继续重试 */
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ws_client_destroy();
    s_supervisor = NULL;
    vTaskDelete(NULL);
}

/* ================== 对外接口 ================== */

esp_err_t volc_conv_ws_start(const volc_conv_credentials_t *cred,
                             volc_conv_ws_audio_cb_t audio_cb,
                             void *user)
{
    if (!cred) return ESP_ERR_INVALID_ARG;
    if (s_ws_run) {
        ESP_LOGW(TAG, "WebSocket 已在运行");
        return ESP_OK;
    }

    memcpy(&s_cred, cred, sizeof(s_cred));
    s_audio_cb   = audio_cb;
    s_audio_user = user;
    s_connected  = false;
    s_need_reconnect = false;
    s_ws_run = true;

    if (xTaskCreate(supervisor_task, "volc_ws_sup", 6144, NULL, 5, &s_supervisor) != pdPASS) {
        ESP_LOGE(TAG, "supervisor 创建失败");
        s_ws_run = false;
        return ESP_FAIL;
    }
    return ESP_OK;
}

void volc_conv_ws_stop(void)
{
    if (!s_ws_run) return;
    s_ws_run = false;
    s_need_reconnect = false;

    /* 等监控任务退出（它负责销毁客户端） */
    for (int i = 0; i < 100 && s_supervisor; ++i) vTaskDelay(pdMS_TO_TICKS(20));
    /* 兜底：若任务未及时退出，直接销毁 */
    ws_client_destroy();

    s_audio_cb   = NULL;
    s_audio_user = NULL;
}

bool volc_conv_ws_is_connected(void)
{
    return s_connected && s_ws && esp_websocket_client_is_connected(s_ws);
}

esp_err_t volc_conv_ws_send_audio(const uint8_t *pcm, size_t len)
{
    if (!volc_conv_ws_is_connected()) return ESP_ERR_INVALID_STATE;
    if (!pcm || len == 0) return ESP_ERR_INVALID_ARG;

    /* base64 编码 PCM */
    size_t b64_cap = ((len + 2) / 3) * 4 + 1;
    /* 估算 JSON 总长，确保 s_tx_buf 容得下 */
    if (b64_cap + 64 > sizeof(s_tx_buf)) {
        ESP_LOGW(TAG, "音频帧过大(%u)，丢弃", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }

    char *b64 = malloc(b64_cap);
    if (!b64) return ESP_ERR_NO_MEM;
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)b64, b64_cap, &olen, pcm, len) != 0) {
        free(b64);
        return ESP_FAIL;
    }
    b64[olen] = '\0';

    int n = snprintf(s_tx_buf, sizeof(s_tx_buf),
                     "{\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", b64);
    free(b64);
    if (n <= 0 || n >= (int)sizeof(s_tx_buf)) {
        return ESP_ERR_NO_MEM;
    }

    int sent = esp_websocket_client_send_text(s_ws, s_tx_buf, n, pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGW(TAG, "上行发送失败");
        if (s_ws_run) s_need_reconnect = true;
        return ESP_FAIL;
    }
    return ESP_OK;
}
