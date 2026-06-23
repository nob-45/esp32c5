/* DynamicRegister 设备注册：
 * POST https://iot-cn-shanghai.iot.volces.com/2021-12-14/DynamicRegister
 * Body: InstanceID/product_key/device_name/random_num/timestamp/auth_type/signature
 * Resp: Result.payload = AES-128-CBC( device_secret, key=product_secret[:16], iv=key[:16] ) base64
 *
 * 拿到后 AES 解密 + PKCS7 去填充 → 明文 device_secret 落 NVS。
 * 后续启动直接从 NVS 读，不再注册。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "nvs.h"

#include <time.h>

#include "mbedtls/aes.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"

#include "cJSON.h"

#include "volc_conv_internal.h"

static const char *TAG = "volc_reg";

/* -------- base64 / hmac 小工具 -------- */

static esp_err_t b64_encode_alloc(const uint8_t *in, size_t in_len, char **out)
{
    size_t need = 0;
    mbedtls_base64_encode(NULL, 0, &need, in, in_len);
    if (need == 0) return ESP_ERR_INVALID_ARG;
    char *buf = malloc(need + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)buf, need, &olen, in, in_len) != 0) {
        free(buf);
        return ESP_FAIL;
    }
    buf[olen] = 0;
    *out = buf;
    return ESP_OK;
}

static esp_err_t b64_decode_alloc(const char *in, size_t in_len, uint8_t **out, size_t *out_len)
{
    size_t need = (in_len / 4 + 1) * 3 + 4;
    uint8_t *buf = malloc(need);
    if (!buf) return ESP_ERR_NO_MEM;
    size_t olen = 0;
    if (mbedtls_base64_decode(buf, need, &olen, (const unsigned char *)in, in_len) != 0) {
        free(buf);
        return ESP_FAIL;
    }
    *out = buf;
    *out_len = olen;
    return ESP_OK;
}

static esp_err_t hmac_sha256_b64(const char *secret, const char *content, char **sig_b64)
{
    unsigned char mac[32];
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) return ESP_FAIL;
    if (mbedtls_md_hmac(info,
                        (const unsigned char *)secret, strlen(secret),
                        (const unsigned char *)content, strlen(content),
                        mac) != 0) {
        return ESP_FAIL;
    }
    return b64_encode_alloc(mac, sizeof(mac), sig_b64);
}

/* -------- NVS -------- */

static esp_err_t nvs_load_credentials(volc_conv_credentials_t *out)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(VOLC_CONV_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;

    size_t len = sizeof(out->device_name);
    err = nvs_get_str(h, VOLC_CONV_NVS_KEY_DEVICE_NAME, out->device_name, &len);
    if (err == ESP_OK) {
        len = sizeof(out->device_secret);
        err = nvs_get_str(h, VOLC_CONV_NVS_KEY_DEVICE_SECRET, out->device_secret, &len);
    }
    nvs_close(h);
    return err;
}

static esp_err_t nvs_save_credentials(const volc_conv_credentials_t *cred)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(VOLC_CONV_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_str(h, VOLC_CONV_NVS_KEY_DEVICE_NAME, cred->device_name);
    if (err == ESP_OK) err = nvs_set_str(h, VOLC_CONV_NVS_KEY_DEVICE_SECRET, cred->device_secret);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

/* -------- 设备名 -------- */

static void make_device_name(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "esp32c5_%02x%02x%02x%02x%02x%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* -------- HTTP 响应聚合 -------- */

#define HTTP_RESP_BUF_MAX 4096

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} http_resp_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_resp_t *r = (http_resp_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && r) {
        size_t needed = r->len + evt->data_len + 1;
        if (needed > r->cap) {
            size_t new_cap = r->cap ? r->cap : 1024;
            while (new_cap < needed) new_cap *= 2;
            if (new_cap > HTTP_RESP_BUF_MAX) return ESP_FAIL;
            char *nb = realloc(r->buf, new_cap);
            if (!nb) return ESP_FAIL;
            r->buf = nb;
            r->cap = new_cap;
        }
        memcpy(r->buf + r->len, evt->data, evt->data_len);
        r->len += evt->data_len;
        r->buf[r->len] = 0;
    }
    return ESP_OK;
}

/* -------- AES-128-CBC PKCS7 解密 -------- */

static esp_err_t aes128_cbc_decrypt(const uint8_t *cipher, size_t cipher_len,
                                    const uint8_t *key16,
                                    uint8_t *plain, size_t *plain_len)
{
    if (cipher_len == 0 || (cipher_len % 16) != 0) return ESP_ERR_INVALID_ARG;
    uint8_t iv[16];
    memcpy(iv, key16, 16);

    mbedtls_aes_context ctx;
    mbedtls_aes_init(&ctx);
    int rc = mbedtls_aes_setkey_dec(&ctx, key16, 128);
    if (rc == 0) {
        rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, cipher_len, iv, cipher, plain);
    }
    mbedtls_aes_free(&ctx);
    if (rc != 0) return ESP_FAIL;

    uint8_t pad = plain[cipher_len - 1];
    if (pad == 0 || pad > 16 || pad > cipher_len) return ESP_ERR_INVALID_RESPONSE;
    *plain_len = cipher_len - pad;
    return ESP_OK;
}

/* -------- DynamicRegister 主流程 -------- */

static esp_err_t do_dynamic_register(volc_conv_credentials_t *out)
{
    make_device_name(out->device_name, sizeof(out->device_name));

    uint32_t random_num = esp_random() & 0x7fffffff;

    /* DynamicRegister 同样要求真实 Unix 毫秒时间戳，依赖外部已 SNTP 同步 */
    time_t now_s = 0;
    time(&now_s);
    if (now_s < 1700000000) {
        ESP_LOGE(TAG, "系统时间异常 now=%lld，未做 SNTP 同步？无法做 DynamicRegister",
                 (long long)now_s);
        return ESP_ERR_INVALID_STATE;
    }
    uint64_t ts_ms = (uint64_t)now_s * 1000ULL;

    char content[512];
    snprintf(content, sizeof(content),
             "auth_type=1&device_name=%s&random_num=%lu&product_key=%s&timestamp=%llu",
             out->device_name,
             (unsigned long)random_num,
             CONFIG_VOLC_CONV_PRODUCT_KEY,
             (unsigned long long)ts_ms);

    char *sig_b64 = NULL;
    esp_err_t err = hmac_sha256_b64(CONFIG_VOLC_CONV_PRODUCT_SECRET, content, &sig_b64);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HMAC 计算失败");
        return err;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "InstanceID", CONFIG_VOLC_CONV_INSTANCE_ID);
    cJSON_AddStringToObject(root, "product_key", CONFIG_VOLC_CONV_PRODUCT_KEY);
    cJSON_AddStringToObject(root, "device_name", out->device_name);
    cJSON_AddNumberToObject(root, "random_num", random_num);
    cJSON_AddNumberToObject(root, "timestamp", (double)ts_ms);
    cJSON_AddNumberToObject(root, "auth_type", 1);
    cJSON_AddStringToObject(root, "signature", sig_b64);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(sig_b64);
    if (!body) return ESP_ERR_NO_MEM;

    ESP_LOGI(TAG, "DynamicRegister POST device_name=%s", out->device_name);
    ESP_LOGD(TAG, "body=%s", body);

    http_resp_t resp = {0};
    esp_http_client_config_t cfg = {
        .url = VOLC_CONV_REGISTER_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = &resp,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        /* 使用 ESP-IDF 内置 CA 证书包做服务器证书校验
         * （需要 sdkconfig 启用 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE，已默认开启）。
         */
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(body);
        if (resp.buf) free(resp.buf);
        return ESP_FAIL;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));

    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP perform 失败: %s", esp_err_to_name(err));
        if (resp.buf) free(resp.buf);
        return err;
    }
    if (status != 200) {
        ESP_LOGE(TAG, "DynamicRegister HTTP %d, body=%s", status, resp.buf ? resp.buf : "(empty)");
        if (resp.buf) free(resp.buf);
        return ESP_FAIL;
    }
    if (!resp.buf) {
        ESP_LOGE(TAG, "响应为空");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "DynamicRegister 响应: %s", resp.buf);

    cJSON *json = cJSON_Parse(resp.buf);
    free(resp.buf);
    resp.buf = NULL;
    if (!json) {
        ESP_LOGE(TAG, "JSON 解析失败");
        return ESP_FAIL;
    }

    cJSON *res_obj = cJSON_GetObjectItemCaseSensitive(json, "Result");
    if (!cJSON_IsObject(res_obj)) {
        ESP_LOGE(TAG, "响应缺少 Result 字段");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    cJSON *payload = cJSON_GetObjectItemCaseSensitive(res_obj, "payload");
    cJSON *rtc_id  = cJSON_GetObjectItemCaseSensitive(res_obj, "rtc_app_id");
    if (!cJSON_IsString(payload) || payload->valuestring == NULL) {
        ESP_LOGE(TAG, "Result.payload 缺失");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    uint8_t *cipher = NULL;
    size_t   cipher_len = 0;
    if (b64_decode_alloc(payload->valuestring, strlen(payload->valuestring),
                         &cipher, &cipher_len) != ESP_OK) {
        ESP_LOGE(TAG, "payload base64 解码失败");
        cJSON_Delete(json);
        return ESP_FAIL;
    }

    uint8_t key16[16] = {0};
    size_t ps_len = strlen(CONFIG_VOLC_CONV_PRODUCT_SECRET);
    memcpy(key16, CONFIG_VOLC_CONV_PRODUCT_SECRET, ps_len < 16 ? ps_len : 16);

    uint8_t plain[256];
    if (cipher_len > sizeof(plain)) {
        ESP_LOGE(TAG, "密文过长 %u", (unsigned)cipher_len);
        free(cipher);
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    size_t plain_len = 0;
    if (aes128_cbc_decrypt(cipher, cipher_len, key16, plain, &plain_len) != ESP_OK) {
        ESP_LOGE(TAG, "AES 解密失败");
        free(cipher);
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    free(cipher);

    if (plain_len >= sizeof(out->device_secret)) {
        ESP_LOGE(TAG, "device_secret 长度 %u 超出缓冲", (unsigned)plain_len);
        cJSON_Delete(json);
        return ESP_FAIL;
    }
    memcpy(out->device_secret, plain, plain_len);
    out->device_secret[plain_len] = 0;

    if (cJSON_IsString(rtc_id) && rtc_id->valuestring) {
        strncpy(out->rtc_app_id, rtc_id->valuestring, sizeof(out->rtc_app_id) - 1);
        out->rtc_app_id[sizeof(out->rtc_app_id) - 1] = 0;
    } else {
        out->rtc_app_id[0] = 0;
    }

    cJSON_Delete(json);

    ESP_LOGI(TAG, "DynamicRegister 成功: device_name=%s device_secret=%s",
             out->device_name, out->device_secret);

    if (nvs_save_credentials(out) != ESP_OK) {
        ESP_LOGW(TAG, "device_secret 落 NVS 失败（非致命）");
    }
    return ESP_OK;
}

/* -------- 对外入口 -------- */

esp_err_t volc_conv_register_obtain(volc_conv_credentials_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));

    if (nvs_load_credentials(out) == ESP_OK &&
        strlen(out->device_secret) > 0 &&
        strlen(out->device_name) > 0) {
        ESP_LOGI(TAG, "已从 NVS 加载凭证 device_name=%s", out->device_name);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "NVS 无凭证，开始 DynamicRegister");
    return do_dynamic_register(out);
}
