/* volc_conv 编排入口：注册 → WebSocket → 启动音频
 *
 * 调用关系：
 *   app_main → volc_conv_app_start
 *      → 后台任务 startup_task
 *           1. volc_conv_register_obtain（NVS/HTTPS）
 *           2. 等到 WS 连接成功
 *           3. volc_conv_audio_start
 *
 * 上行回调：volc_conv_audio.send_cb → volc_conv_ws_send_audio
 * 下行回调：volc_conv_ws.audio_cb  → volc_conv_audio_play
 */

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "volc_conv_app.h"
#include "volc_conv_internal.h"

static const char *TAG = "volc_conv";

static volc_conv_credentials_t s_cred;
static TaskHandle_t            s_startup_task = NULL;
static volatile bool           s_running = false;

/* WebSocket 下行音频 → 喇叭 */
static void on_ws_audio(const uint8_t *pcm, size_t len, void *user)
{
    (void)user;
    volc_conv_audio_play(pcm, len);
}

/* 麦克风 → WebSocket 上行 */
static esp_err_t on_mic_frame(const uint8_t *pcm, size_t len)
{
    if (!volc_conv_ws_is_session_ready()) return ESP_ERR_INVALID_STATE;
    return volc_conv_ws_send_audio(pcm, len);
}

static void startup_task(void *arg)
{
    /* 1. 拿凭证（NVS 或 DynamicRegister） */
    esp_err_t err = volc_conv_register_obtain(&s_cred);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "获取设备凭证失败: %s，退出", esp_err_to_name(err));
        goto end;
    }

    /* 2. 启 WebSocket */
    err = volc_conv_ws_start(&s_cred, on_ws_audio, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WebSocket 启动失败: %s", esp_err_to_name(err));
        goto end;
    }

    /* 3. 等 WebSocket + session.update 完成。
     *    ws 模块已禁用 esp_websocket_client 内置重连，改由 supervisor 用新时间戳/
     *    新签名重建客户端；这里等待完整会话就绪，而不是只等 TCP/WS 连接成功。
     *    如果过早启动麦克风，input_audio_buffer.append 可能早于 session.update 到达，
     *    服务端会不知道 PCM16/16k 与 server_vad 配置，表现为空响应或对话无声音。 */
    int wait_ms = 0;
    while (!volc_conv_ws_is_session_ready() && wait_ms < 120000 && s_running) {
        vTaskDelay(pdMS_TO_TICKS(200));
        wait_ms += 200;
    }
    if (!volc_conv_ws_is_session_ready()) {
        ESP_LOGE(TAG, "WebSocket 会话 120s 内未就绪，放弃启动音频");
        goto end;
    }
    ESP_LOGI(TAG, "WebSocket 会话已就绪(耗时约 %d ms)，启动音频", wait_ms);

    /* 4. 启动音频采集/播放 */
    err = volc_conv_audio_start(on_mic_frame);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "音频启动失败: %s", esp_err_to_name(err));
        volc_conv_ws_stop();
        goto end;
    }

    ESP_LOGI(TAG, "对话客户端就绪。开口说话即可");

end:
    s_startup_task = NULL;
    vTaskDelete(NULL);
}

bool volc_conv_app_start(void)
{
    if (s_running) {
        ESP_LOGW(TAG, "已在运行");
        return true;
    }
    s_running = true;

    if (xTaskCreate(startup_task, "volc_conv_init", 6144, NULL, 5, &s_startup_task) != pdPASS) {
        ESP_LOGE(TAG, "startup_task 创建失败");
        s_running = false;
        return false;
    }
    return true;
}

void volc_conv_app_audio_selftest(void)
{
    volc_conv_audio_selftest();
}

void volc_conv_app_stop(void)
{
    if (!s_running) return;
    s_running = false;

    volc_conv_audio_stop();
    volc_conv_ws_stop();

    /* 等启动任务退出 */
    for (int i = 0; i < 50 && s_startup_task; ++i) vTaskDelay(pdMS_TO_TICKS(20));
}