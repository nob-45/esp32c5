/* volc_conv 组件内部共享定义 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== 火山引擎账号配置（来自 menuconfig 或硬编码） ===== */
#ifndef CONFIG_VOLC_CONV_INSTANCE_ID
#define CONFIG_VOLC_CONV_INSTANCE_ID    "6a266cccc0c4689234aef4fc"
#endif
#ifndef CONFIG_VOLC_CONV_PRODUCT_KEY
#define CONFIG_VOLC_CONV_PRODUCT_KEY    "6a323b6b7369bf7d5c01591f"
#endif
#ifndef CONFIG_VOLC_CONV_PRODUCT_SECRET
#define CONFIG_VOLC_CONV_PRODUCT_SECRET "fc0a778fa0f339302a2089f6"
#endif
#ifndef CONFIG_VOLC_CONV_BOT_ID
/* 智能体 ID，从智能体管理页获取，必须与产品已关联 */
#define CONFIG_VOLC_CONV_BOT_ID         "botkT33ubNjl"
#endif

/* DynamicRegister HTTPS 接口 */
#define VOLC_CONV_REGISTER_HOST         "iot-cn-shanghai.iot.volces.com"
#define VOLC_CONV_REGISTER_PATH         "/2021-12-14/DynamicRegister?Action=DynamicRegister&Version=2021-12-14"
#define VOLC_CONV_REGISTER_URL          "https://" VOLC_CONV_REGISTER_HOST VOLC_CONV_REGISTER_PATH

/* WebSocket 网关 */
#define VOLC_CONV_WS_HOST               "ai-gateway.vei.volces.com"
#define VOLC_CONV_WS_PATH               "/v1/realtime"

/* NVS 存储 */
#define VOLC_CONV_NVS_NAMESPACE         "volc_conv"
#define VOLC_CONV_NVS_KEY_DEVICE_SECRET "dev_secret"
#define VOLC_CONV_NVS_KEY_DEVICE_NAME   "dev_name"

/* 音频参数：火山智能对话默认 PCM16, 16kHz, 单声道 */
#define VOLC_CONV_AUDIO_SAMPLE_RATE_HZ  16000
#define VOLC_CONV_AUDIO_FRAME_MS        20
#define VOLC_CONV_AUDIO_FRAME_SAMPLES   ((VOLC_CONV_AUDIO_SAMPLE_RATE_HZ * VOLC_CONV_AUDIO_FRAME_MS) / 1000)
#define VOLC_CONV_AUDIO_FRAME_BYTES     (VOLC_CONV_AUDIO_FRAME_SAMPLES * 2)

/* GPIO（C5 上常用、未与 I2C/UART 冲突的脚。要按你板子实际改） */
#ifndef CONFIG_VOLC_CONV_MIC_ADC_GPIO
#define CONFIG_VOLC_CONV_MIC_ADC_GPIO   1   /* GPIO1 = ADC1_CH0 */
#endif
#ifndef CONFIG_VOLC_CONV_SPK_PWM_GPIO
#define CONFIG_VOLC_CONV_SPK_PWM_GPIO   4
#endif

/* ===== Register 模块 ===== */

typedef struct {
    char device_name[64];      /* 自动生成（基于 MAC） */
    char device_secret[128];   /* 解密后的明文 */
    char rtc_app_id[64];       /* 仅 RTC 模式用，本工程暂不使用 */
} volc_conv_credentials_t;

/* 准备凭证：先尝试从 NVS 读，没有就调用 DynamicRegister 注册并落盘 */
esp_err_t volc_conv_register_obtain(volc_conv_credentials_t *out);

/* ===== WebSocket 模块 ===== */

/* 收到一段服务端 PCM16 音频（已 base64 解码）回调
 * data 生命周期仅限回调内有效，需要在回调里立即拷走或直接送播放 */
typedef void (*volc_conv_ws_audio_cb_t)(const uint8_t *pcm, size_t len, void *user);

esp_err_t volc_conv_ws_start(const volc_conv_credentials_t *cred,
                             volc_conv_ws_audio_cb_t audio_cb,
                             void *user);
void      volc_conv_ws_stop(void);
bool      volc_conv_ws_is_connected(void);

/* 发送一段本地采集的 PCM16 音频（内部 base64 后封装为 input_audio_buffer.append 事件） */
esp_err_t volc_conv_ws_send_audio(const uint8_t *pcm, size_t len);

/* ===== Audio 模块 ===== */

/* 启动 ADC 麦克风采集任务，每 20ms 调一次 send_cb 上送 PCM16 */
typedef esp_err_t (*volc_conv_audio_send_cb_t)(const uint8_t *pcm, size_t len);

esp_err_t volc_conv_audio_start(volc_conv_audio_send_cb_t send_cb);
void      volc_conv_audio_stop(void);

/* 把下行 PCM16 推到播放队列 */
esp_err_t volc_conv_audio_play(const uint8_t *pcm, size_t len);

#ifdef __cplusplus
}
#endif