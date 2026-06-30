/* 音频采集 / 播放（仅作流程验证，音质很差）
 *
 * 采集：使用 ESP32-C5 ADC continuous mode，固定 16kHz 单声道 12bit。
 *      每帧 320 个采样（20ms），转换为 PCM16（中心化处理）后回调 send_cb。
 *
 * 播放：使用 LEDC PWM（高频载波 + PCM16 → 8bit 占空比映射）。
 *      下行音频塞进环形队列，专用任务每 1/16000s 取一个采样写占空比。
 *      仅供"听个响"，不能作为正式喇叭驱动。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/ringbuf.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_adc/adc_continuous.h"
#include "driver/ledc.h"
#include "driver/gptimer.h"

#include "volc_conv_internal.h"

static const char *TAG = "volc_audio";

/* ================== 采集 ================== */

#define MIC_ADC_UNIT          ADC_UNIT_1
#define MIC_ADC_CHANNEL       ADC_CHANNEL_0   /* GPIO1 */
#define MIC_ADC_BIT_WIDTH     ADC_BITWIDTH_12
#define MIC_ADC_ATTEN         ADC_ATTEN_DB_12

/* ADC continuous 每帧字节数：320 sample × sizeof(adc_digi_output_data_t)(=4) */
#define MIC_ADC_FRAME_BYTES   (VOLC_CONV_AUDIO_FRAME_SAMPLES * SOC_ADC_DIGI_RESULT_BYTES)

static adc_continuous_handle_t  s_adc_handle = NULL;
static TaskHandle_t             s_mic_task   = NULL;
static volc_conv_audio_send_cb_t s_send_cb   = NULL;
static volatile bool            s_mic_running = false;

/* 累积 MIC_SEND_FRAMES 个 20ms 帧(共 100ms)再上行一次，
 * 把 WS 发送频率从 50 帧/秒降到 10 帧/秒，避免 TLS 写缓冲被打满
 * 导致 transport_poll_write 超时断连 */
#define MIC_SEND_FRAMES   5
#define MIC_ACCUM_SAMPLES (VOLC_CONV_AUDIO_FRAME_SAMPLES * MIC_SEND_FRAMES)

static void mic_task(void *arg)
{
    uint8_t  *adc_buf = malloc(MIC_ADC_FRAME_BYTES);
    int16_t  *pcm_buf = malloc(MIC_ACCUM_SAMPLES * sizeof(int16_t));
    size_t    pcm_fill = 0;  /* 累积区已填入的采样数 */
    if (!adc_buf || !pcm_buf) {
        ESP_LOGE(TAG, "麦克风缓冲分配失败");
        goto end;
    }

    while (s_mic_running) {
        uint32_t got = 0;
        esp_err_t err = adc_continuous_read(s_adc_handle, adc_buf, MIC_ADC_FRAME_BYTES, &got, 100);
        if (err == ESP_ERR_TIMEOUT || got == 0) {
            continue;
        }
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "ADC read 失败: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        /* 把 12bit ADC 中心化转 PCM16，追加到累积区 */
        size_t n = got / SOC_ADC_DIGI_RESULT_BYTES;
        if (n > VOLC_CONV_AUDIO_FRAME_SAMPLES) n = VOLC_CONV_AUDIO_FRAME_SAMPLES;
        for (size_t i = 0; i < n && pcm_fill < MIC_ACCUM_SAMPLES; ++i) {
            adc_digi_output_data_t *d = (adc_digi_output_data_t *)&adc_buf[i * SOC_ADC_DIGI_RESULT_BYTES];
            int raw = d->type2.data;            /* 12bit 0~4095 */
            int centered = raw - 2048;          /* -2048 ~ +2047 */
            pcm_buf[pcm_fill++] = (int16_t)(centered * 16); /* 放大到 ~16bit 范围 */
        }

        /* 攒够 100ms 再一次性上行 */
        if (pcm_fill >= MIC_ACCUM_SAMPLES) {
            if (s_send_cb) {
                s_send_cb((const uint8_t *)pcm_buf, pcm_fill * sizeof(int16_t));
            }
            pcm_fill = 0;
        }
    }

end:
    if (adc_buf) free(adc_buf);
    if (pcm_buf) free(pcm_buf);
    s_mic_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t mic_start(void)
{
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = MIC_ADC_FRAME_BYTES * 4,
        .conv_frame_size    = MIC_ADC_FRAME_BYTES,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_new_handle(&handle_cfg, &s_adc_handle), TAG, "adc_new");

    adc_digi_pattern_config_t pattern = {
        .atten     = MIC_ADC_ATTEN,
        .channel   = MIC_ADC_CHANNEL & 0x7,
        .unit      = MIC_ADC_UNIT,
        .bit_width = MIC_ADC_BIT_WIDTH,
    };
    adc_continuous_config_t adc_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = VOLC_CONV_AUDIO_SAMPLE_RATE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    ESP_RETURN_ON_ERROR(adc_continuous_config(s_adc_handle, &adc_cfg), TAG, "adc_cfg");
    ESP_RETURN_ON_ERROR(adc_continuous_start(s_adc_handle), TAG, "adc_start");

    s_mic_running = true;
    if (xTaskCreate(mic_task, "volc_mic", 4096, NULL, 12, &s_mic_task) != pdPASS) {
        s_mic_running = false;
        adc_continuous_stop(s_adc_handle);
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "麦克风启动 GPIO%d, 16kHz", CONFIG_VOLC_CONV_MIC_ADC_GPIO);
    return ESP_OK;
}

static void mic_stop(void)
{
    s_mic_running = false;
    /* 等任务退出 */
    for (int i = 0; i < 50 && s_mic_task; ++i) vTaskDelay(pdMS_TO_TICKS(10));
    if (s_adc_handle) {
        adc_continuous_stop(s_adc_handle);
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
    }
}

/* ================== 播放（LEDC PWM + GPTimer 16kHz 推送） ================== */

#define SPK_LEDC_TIMER       LEDC_TIMER_0
#define SPK_LEDC_CHANNEL     LEDC_CHANNEL_0
#define SPK_LEDC_MODE        LEDC_LOW_SPEED_MODE
#define SPK_LEDC_DUTY_RES    LEDC_TIMER_8_BIT
#define SPK_LEDC_FREQ_HZ     78125            /* 高于人耳听觉，PWM 载波 */
#define SPK_RING_BYTES       (16 * 1024)       /* ~0.5s 缓冲 */

static RingbufHandle_t  s_spk_rb = NULL;
static gptimer_handle_t s_spk_timer = NULL;

static bool IRAM_ATTR spk_timer_cb(gptimer_handle_t timer, const gptimer_alarm_event_data_t *evt, void *user)
{
    /* 中断里只取一个 16bit 采样并写 LEDC duty。
     * xRingbufferReceiveUpToFromISR 返回连续地址。 */
    BaseType_t hpw = pdFALSE;
    size_t got = 0;
    int16_t *p = (int16_t *)xRingbufferReceiveUpToFromISR(s_spk_rb, &got, sizeof(int16_t));
    int16_t s = 0;
    if (p && got >= 2) {
        s = *p;
        vRingbufferReturnItemFromISR(s_spk_rb, p, &hpw);
    }
    /* PCM16 → 8bit 占空比：[-32768,32767] → [0,255] */
    uint32_t duty = (uint32_t)((s + 32768) >> 8);
    if (duty > 255) duty = 255;
    ledc_set_duty(SPK_LEDC_MODE, SPK_LEDC_CHANNEL, duty);
    ledc_update_duty(SPK_LEDC_MODE, SPK_LEDC_CHANNEL);

    return hpw == pdTRUE;
}

static esp_err_t spk_start(void)
{
    /* LEDC */
    ledc_timer_config_t tcfg = {
        .speed_mode      = SPK_LEDC_MODE,
        .timer_num       = SPK_LEDC_TIMER,
        .duty_resolution = SPK_LEDC_DUTY_RES,
        .freq_hz         = SPK_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc_timer");

    ledc_channel_config_t ccfg = {
        .gpio_num   = CONFIG_VOLC_CONV_SPK_PWM_GPIO,
        .speed_mode = SPK_LEDC_MODE,
        .channel    = SPK_LEDC_CHANNEL,
        .timer_sel  = SPK_LEDC_TIMER,
        .duty       = 128,
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc_ch");

    /* 环形缓冲 */
    s_spk_rb = xRingbufferCreate(SPK_RING_BYTES, RINGBUF_TYPE_BYTEBUF);
    if (!s_spk_rb) return ESP_ERR_NO_MEM;

    /* 16kHz 采样 GPTimer */
    gptimer_config_t tmcfg = {
        .clk_src       = GPTIMER_CLK_SRC_DEFAULT,
        .direction     = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,  /* 1us */
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&tmcfg, &s_spk_timer), TAG, "gptimer_new");
    gptimer_alarm_config_t alarm = {
        .alarm_count = 1000000 / VOLC_CONV_AUDIO_SAMPLE_RATE_HZ,  /* 62.5us */
        .reload_count = 0,
        .flags.auto_reload_on_alarm = true,
    };
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_spk_timer, &alarm), TAG, "gptimer_alarm");

    gptimer_event_callbacks_t cbs = {
        .on_alarm = spk_timer_cb,
    };
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_spk_timer, &cbs, NULL), TAG, "gptimer_cbs");
    ESP_RETURN_ON_ERROR(gptimer_enable(s_spk_timer), TAG, "gptimer_enable");
    ESP_RETURN_ON_ERROR(gptimer_start(s_spk_timer), TAG, "gptimer_start");

    ESP_LOGI(TAG, "喇叭启动 GPIO%d, PWM=%dHz, 采样=16kHz",
             CONFIG_VOLC_CONV_SPK_PWM_GPIO, SPK_LEDC_FREQ_HZ);
    return ESP_OK;
}

static void spk_stop(void)
{
    if (s_spk_timer) {
        gptimer_stop(s_spk_timer);
        gptimer_disable(s_spk_timer);
        gptimer_del_timer(s_spk_timer);
        s_spk_timer = NULL;
    }
    ledc_stop(SPK_LEDC_MODE, SPK_LEDC_CHANNEL, 0);
    if (s_spk_rb) {
        vRingbufferDelete(s_spk_rb);
        s_spk_rb = NULL;
    }
}

/* ================== 对外接口 ================== */

esp_err_t volc_conv_audio_start(volc_conv_audio_send_cb_t send_cb)
{
    s_send_cb = send_cb;

    esp_err_t err = spk_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "喇叭启动失败: %s", esp_err_to_name(err));
        spk_stop();
        return err;
    }

    err = mic_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "麦克风启动失败: %s", esp_err_to_name(err));
        spk_stop();
        return err;
    }
    return ESP_OK;
}

void volc_conv_audio_stop(void)
{
    mic_stop();
    spk_stop();
    s_send_cb = NULL;
}

esp_err_t volc_conv_audio_play(const uint8_t *pcm, size_t len)
{
    if (!s_spk_rb || !pcm || len == 0) return ESP_ERR_INVALID_STATE;
    BaseType_t ok = xRingbufferSend(s_spk_rb, pcm, len, 0);
    if (ok != pdTRUE) {
        ESP_LOGD(TAG, "喇叭缓冲已满，丢弃 %u 字节", (unsigned)len);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* ================== 硬件自测（不依赖云端） ==================
 * 流程：
 *   1) 喇叭测试：依次播放 440Hz / 880Hz 两段正弦提示音，每段 1 秒。
 *      若喇叭/功放/接线正常，应能听到两声"嘀"。
 *   2) 麦克风测试：采集 5 秒，每 200ms 统计一帧的峰峰值(p2p)与直流均值，
 *      并打印进度条。对着麦克风说话/吹气时 p2p 应明显增大。
 *
 * 该函数会阻塞约 7 秒，仅供上电后单独调用，不与正常对话流程并存。
 */
void volc_conv_audio_selftest(void)
{
    ESP_LOGW(TAG, "========== 音频硬件自测开始 ==========");

    /* ---------- 1. 喇叭测试 ---------- */
    if (spk_start() != ESP_OK) {
        ESP_LOGE(TAG, "[自测] 喇叭初始化失败，跳过喇叭测试");
    } else {
        const int tone_freqs[] = {440, 880};
        for (int t = 0; t < 2; ++t) {
            int freq = tone_freqs[t];
            ESP_LOGW(TAG, "[自测] 播放 %dHz 提示音 1 秒，请注意听喇叭...", freq);
            /* 分块生成正弦并送入播放环形队列，1 秒 = 16000 采样 */
            const int total = VOLC_CONV_AUDIO_SAMPLE_RATE_HZ; /* 1s */
            const int chunk = 320;
            int16_t buf[320];
            for (int done = 0; done < total; done += chunk) {
                int nthis = (total - done) < chunk ? (total - done) : chunk;
                for (int i = 0; i < nthis; ++i) {
                    double ph = 2.0 * M_PI * freq * (double)(done + i) / VOLC_CONV_AUDIO_SAMPLE_RATE_HZ;
                    buf[i] = (int16_t)(sin(ph) * 12000.0); /* 适中音量 */
                }
                /* 等待队列有空间再写，避免丢样 */
                int retry = 0;
                while (volc_conv_audio_play((const uint8_t *)buf, nthis * sizeof(int16_t)) != ESP_OK
                       && retry++ < 100) {
                    vTaskDelay(pdMS_TO_TICKS(5));
                }
            }
            vTaskDelay(pdMS_TO_TICKS(300)); /* 两声之间停顿 */
        }
        ESP_LOGW(TAG, "[自测] 喇叭测试结束。若没听到声音，请检查: 功放使能脚/喇叭接线/SPK GPIO%d/音量",
                 CONFIG_VOLC_CONV_SPK_PWM_GPIO);
        spk_stop();
    }

    /* ---------- 2. 麦克风测试 ---------- */
    /* 直接用 ADC continuous，不启动上行任务 */
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = MIC_ADC_FRAME_BYTES * 4,
        .conv_frame_size    = MIC_ADC_FRAME_BYTES,
    };
    if (adc_continuous_new_handle(&handle_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "[自测] 麦克风 ADC 初始化失败");
        goto done;
    }
    adc_digi_pattern_config_t pattern = {
        .atten     = MIC_ADC_ATTEN,
        .channel   = MIC_ADC_CHANNEL & 0x7,
        .unit      = MIC_ADC_UNIT,
        .bit_width = MIC_ADC_BIT_WIDTH,
    };
    adc_continuous_config_t adc_cfg = {
        .pattern_num    = 1,
        .adc_pattern    = &pattern,
        .sample_freq_hz = VOLC_CONV_AUDIO_SAMPLE_RATE_HZ,
        .conv_mode      = ADC_CONV_SINGLE_UNIT_1,
        .format         = ADC_DIGI_OUTPUT_FORMAT_TYPE2,
    };
    if (adc_continuous_config(s_adc_handle, &adc_cfg) != ESP_OK ||
        adc_continuous_start(s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "[自测] 麦克风 ADC 启动失败");
        adc_continuous_deinit(s_adc_handle);
        s_adc_handle = NULL;
        goto done;
    }

    ESP_LOGW(TAG, "[自测] 麦克风采集 5 秒，请对着麦克风说话/吹气...");
    uint8_t *adc_buf = malloc(MIC_ADC_FRAME_BYTES);
    if (adc_buf) {
        for (int frame = 0; frame < 25; ++frame) { /* 25 × 200ms = 5s */
            int rawmin = 4095, rawmax = 0;
            long rawsum = 0;
            int cnt = 0;
            /* 200ms 内连续读多帧统计 */
            int64_t t0 = esp_log_timestamp();
            while (esp_log_timestamp() - t0 < 200) {
                uint32_t got = 0;
                if (adc_continuous_read(s_adc_handle, adc_buf, MIC_ADC_FRAME_BYTES, &got, 100) != ESP_OK)
                    continue;
                size_t n = got / SOC_ADC_DIGI_RESULT_BYTES;
                for (size_t i = 0; i < n; ++i) {
                    adc_digi_output_data_t *d = (adc_digi_output_data_t *)&adc_buf[i * SOC_ADC_DIGI_RESULT_BYTES];
                    int raw = d->type2.data;
                    if (raw < rawmin) rawmin = raw;
                    if (raw > rawmax) rawmax = raw;
                    rawsum += raw;
                    cnt++;
                }
            }
            if (cnt > 0) {
                int p2p = rawmax - rawmin;
                int mean = (int)(rawsum / cnt);
                /* 简单条形图 */
                int bars = p2p / 50; if (bars > 40) bars = 40;
                char bar[44]; memset(bar, '#', bars); bar[bars] = 0;
                ESP_LOGI(TAG, "[自测][MIC] 帧%2d 直流均值=%4d 峰峰值=%4d |%s",
                         frame, mean, p2p, bar);
            } else {
                ESP_LOGW(TAG, "[自测][MIC] 帧%2d 没读到采样(ADC 无数据)", frame);
            }
        }
        free(adc_buf);
    }
    adc_continuous_stop(s_adc_handle);
    adc_continuous_deinit(s_adc_handle);
    s_adc_handle = NULL;

    ESP_LOGW(TAG, "[自测] 麦克风测试结束。判断标准:");
    ESP_LOGW(TAG, "[自测]   - 安静时峰峰值小、说话时峰峰值明显变大 => 麦克风正常");
    ESP_LOGW(TAG, "[自测]   - 峰峰值始终≈0 或恒定不变 => 麦克风没接好/MIC GPIO%d 错误/无偏置",
             CONFIG_VOLC_CONV_MIC_ADC_GPIO);

done:
    ESP_LOGW(TAG, "========== 音频硬件自测结束 ==========");
}
