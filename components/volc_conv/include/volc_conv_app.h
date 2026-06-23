/* 火山引擎"硬件对话智能体"客户端
 *
 * 走标准 WebSocket 协议接入：
 * 1. 设备首次启动调用 DynamicRegister 用 ProductSecret 换取 device_secret 并落 NVS。
 * 2. 用 device_secret 计算 HMAC-SHA256 签名建立 wss 长连接。
 * 3. 麦克风采 16k PCM16 → base64 → input_audio_buffer.append 上行。
 * 4. 下行 response.audio.delta → base64 解码 → 播放到喇叭。
 *
 * 仅作流程验证，使用 ESP32-C5 ADC + LEDC PWM 凑合两根线的模拟麦克风/喇叭。
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动整个语音对话客户端：注册 → 建连 → 启动音频采集/播放
 * 调用前必须确保 Wi-Fi 已连接、NVS 已 init。
 * 返回 true 表示后台任务已发起；具体连接结果通过日志查看。
 */
bool volc_conv_app_start(void);

/* 停止并释放所有资源 */
void volc_conv_app_stop(void);

#ifdef __cplusplus
}
#endif