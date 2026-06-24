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

/* 会话配置握手状态：
 *  收到 session.created 后置 s_session_ready，由 supervisor 任务发送
 *  session.update（不能在 WS 事件回调里发，否则与发送锁竞争）。 */
static volatile bool           s_session_ready = false;       /* 已收到 session.created */
static volatile bool           s_session_update_sent = false; /* 已发送 session.update */

/* 鉴权 Header 缓冲：client 在 connect 时引用该指针，需在连接期间保持有效。
 * supervisor 单线程串行重建，故静态安全。 */
static char s_headers[1024];

/* 上行 base64+JSON 复用缓冲 */
static char s_tx_buf[8192];

/* 下行 WS 分片重组缓冲（服务端音频消息常被拆成多个 continuation 帧） */
static char  *s_rx_buf = NULL;
static size_t s_rx_len = 0;
static size_t s_rx_cap = 0;

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

    /* WebSocket 建连时间戳用【秒】：文档明确强调与 DynamicRegister（毫秒）不同，
     * 且服务端有 1 分钟时间窗校验（错误码 30070216/30070217）。 */
    uint64_t ts_sec = (uint64_t)time(NULL);
    uint32_t rnd = esp_random() & 0x7fffffff;
    char random_num[16];
    snprintf(random_num, sizeof(random_num), "%lu", (unsigned long)rnd);

    /* 规范串（WebSocket 建连专用，与 DynamicRegister 不同）：
     * auth_type&device_name&random_num&product_key&timestamp&instance_id
     * 必须包含 instance_id，签名密钥用 device_secret。 */
    char canon[512];
    snprintf(canon, sizeof(canon),
             "auth_type=1&device_name=%s&random_num=%s&product_key=%s&timestamp=%llu&instance_id=%s",
             cred->device_name,
             random_num,
             CONFIG_VOLC_CONV_PRODUCT_KEY,
             (unsigned long long)ts_sec,
             CONFIG_VOLC_CONV_INSTANCE_ID);

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
                     (unsigned long long)ts_sec,
                     CONFIG_VOLC_CONV_INSTANCE_ID,
                     mac_str,
                     sig);
    if (n <= 0 || n >= (int)sizeof(s_headers)) {
        ESP_LOGE(TAG, "Header 缓冲不足");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "鉴权 Header 就绪 ts_sec=%llu device=%s",
             (unsigned long long)ts_sec, cred->device_name);
    return ESP_OK;
}

/* ================== 下行处理 ================== */

/* 解析服务端文本帧。关心 response.audio.delta（base64 PCM16）。 */
static void handle_server_text(const char *data, int len)
{
    cJSON *root = cJSON_ParseWithLength(data, len);
    if (!root) {
        /* 非 JSON 文本帧，截断打印便于诊断 */
        ESP_LOGW(TAG, "下行非JSON文本(%d字节): %.*s", len,
                 len > 200 ? 200 : len, data);
        return;
    }

    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (cJSON_IsString(type) && type->valuestring) {
        /* 诊断：打印所有下行消息类型（audio.delta 太频繁单独处理） */
        if (strcmp(type->valuestring, "response.audio.delta") != 0) {
            ESP_LOGI(TAG, "下行消息 type=%s", type->valuestring);
        }
        if (strcmp(type->valuestring, "session.created") == 0) {
            /* 握手第一步完成：置标志，交给 supervisor 任务发 session.update。
             * 不能在此回调（WS 任务上下文）里直接发送，会与写锁竞争超时。 */
            s_session_ready = true;
        } else if (strcmp(type->valuestring, "response.audio.delta") == 0) {
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

/* ================== 会话配置（session.update） ================== */

/* 发送 session.update 配置会话。
 *
 * 火山 Realtime（OpenAI 兼容）协议要求在 session.created 后，由客户端发送
 * session.update 显式声明上/下行音频格式与服务端 VAD（turn_detection）。
 * 否则服务端不知道何时判定一轮说话结束、用什么格式解码上行 PCM，
 * 结果就是产生一个空响应——只有 response.audio.done 而没有任何 audio.delta。
 *
 * 本工程音频固定 PCM16 / 16kHz / 单声道，turn_detection 用 server_vad。 */
static esp_err_t send_session_update(void)
{
    int n = snprintf(s_tx_buf, sizeof(s_tx_buf),
        "{\"type\":\"session.update\",\"session\":{"
        "\"input_audio_format\":\"pcm16\","
        "\"output_audio_format\":\"pcm16\","
        "\"turn_detection\":{\"type\":\"server_vad\"}"
        "}}");
    if (n <= 0 || n >= (int)sizeof(s_tx_buf)) return ESP_ERR_NO_MEM;

    int sent = esp_websocket_client_send_text(s_ws, s_tx_buf, n, pdMS_TO_TICKS(1000));
    if (sent < 0) {
        ESP_LOGW(TAG, "session.update 发送失败");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "已发送 session.update（pcm16/16k + server_vad）");
    return ESP_OK;
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
        /* 新连接：重置会话握手状态，等待服务端 session.created */
        s_session_ready = false;
        s_session_update_sent = false;
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "WebSocket 断开");
        s_connected = false;
        s_session_ready = false;
        s_session_update_sent = false;
        if (s_ws_run) s_need_reconnect = true;  /* 交给 supervisor 重建 */
        break;

    case WEBSOCKET_EVENT_DATA:
        if (d->op_code == 0x08) {            /* close frame */
            ESP_LOGW(TAG, "收到 close 帧");
            s_connected = false;
            if (s_ws_run) s_need_reconnect = true;
        } else if ((d->op_code == 0x01 || d->op_code == 0x00) && d->data_len > 0) {
            /* text(0x01) 或 continuation(0x00) 帧：按 payload_offset/payload_len 重组。
             * 火山下行音频消息常超过单帧大小被拆片，必须拼回完整 JSON 再解析。 */
            int total = d->payload_len;   /* 整条消息总长 */
            int off   = d->payload_offset;

            if (off == 0) {               /* 新消息开始 */
                s_rx_len = 0;
                if (s_rx_cap < (size_t)total + 1) {
                    char *p = realloc(s_rx_buf, total + 1);
                    if (!p) {
                        ESP_LOGE(TAG, "下行缓冲分配失败(%d)", total);
                        s_rx_cap = 0;
                        break;
                    }
                    s_rx_buf = p;
                    s_rx_cap = total + 1;
                }
            }
            if (s_rx_buf && s_rx_len + d->data_len <= s_rx_cap) {
                memcpy(s_rx_buf + s_rx_len, d->data_ptr, d->data_len);
                s_rx_len += d->data_len;
            }
            /* 收齐则解析 */
            if (s_rx_buf && s_rx_len >= (size_t)total && total > 0) {
                s_rx_buf[s_rx_len] = '\0';
                handle_server_text(s_rx_buf, s_rx_len);
                s_rx_len = 0;
            }
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
        /* WebSocket 层 ping/pong 保活：避免长时间空闲被服务端断开，
         * 并能及时探测半关闭的死连接（之前 ~160s 后 transport_poll_write 超时）。 */
        .ping_interval_sec         = 10,
        .pingpong_timeout_sec      = 20,
        .disable_pingpong_discon   = false,
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

        /* 握手第二步：收到 session.created 后在本任务上下文发送 session.update。
         * 放这里而非 WS 事件回调，是为了避开与下行写操作竞争发送锁。 */
        if (s_session_ready && !s_session_update_sent && volc_conv_ws_is_connected()) {
            if (send_session_update() == ESP_OK) {
                s_session_update_sent = true;
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

    /* 注意：发送失败多为下行音频占用写锁导致的临时超时，并非真断线。
     * 音频是实时流，丢一帧无妨，绝不能因此触发重连（否则陷入重连死循环）。
     * 真正的断线由 DISCONNECTED/ERROR/close 事件负责处理。 */
    int sent = esp_websocket_client_send_text(s_ws, s_tx_buf, n, pdMS_TO_TICKS(200));
    if (sent < 0) {
        return ESP_FAIL;   /* 静默丢帧，不重连 */
    }
    return ESP_OK;
}
