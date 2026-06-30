/**
 * @file bme69x_port.c
 * @brief ESP-IDF I2C 适配层 - 把 Bosch 官方 BME69x 驱动接到 ESP-IDF i2c_master
 *
 * 关键设计：
 *  - I2C 频率 100 kHz (与 SensairShuttle factory_demo 一致)
 *  - read() 用 i2c_master_write_read_device()，无 STOP 重新发 Sr，
 *    完美匹配 BME69x 寄存器读协议 (写 reg_addr 字节 -> Sr -> 读 N 字节)
 *  - write() 用 i2c_master_write()，多字节连续写
 *  - delay_us() 用 ets_delay_us()
 *  - intf_ptr 用来传 i2c port number
 */
#include "bme69x_port.h"
#include "bme69x.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp32c5/rom/ets_sys.h"   /* IDF5.x: ets_delay_us() 仍在 ets_sys.h */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme69x_port";

static struct bme69x_dev s_dev;     /* Bosch 官方设备结构 */
static bool s_initialized = false;
/* 运行时探测到的 I2C 地址（0x76 或 0x77）。必须是可变的，bme69x_i2c_read/write
 * 都要通过它收发。如果直接用 BME69X_I2C_ADDR 宏, 扫描出来的 0x77
 * 形同虚设 - 实际访问仍然走 0x76, 100% 失败。 */
static uint8_t s_i2c_addr = 0;

/* ===== Bosch 官方驱动要求实现的 3 个回调 ===== */

static int8_t bme69x_i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    i2c_port_t port = (i2c_port_t)(intptr_t)intf_ptr;
    /* 必须用运行时探测出来的 s_i2c_addr, 不能直接用 BME69X_I2C_ADDR 宏
       (那是编译期固定的 0x76, 硬件在 0x77 时会全部 ACK fail) */
    if (s_i2c_addr == 0) {
        return BME69X_E_COM_FAIL;
    }
    /* i2c_master_write_read_device 内部自动产生 Sr，不发 STOP，
       这正是 BME69x 寄存器读协议要的"写 reg_addr -> Sr -> 读 N 字节" */
    esp_err_t err = i2c_master_write_read_device(port,
                                                  s_i2c_addr,
                                                  &reg_addr, 1,
                                                  reg_data, length,
                                                  pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "i2c_read reg=0x%02x len=%lu failed: %s",
                 reg_addr, (unsigned long)length, esp_err_to_name(err));
        return (int8_t)err;  /* 非 0 = BME69X_E_COM_FAIL */
    }
    return 0;
}

static int8_t bme69x_i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr)
{
    i2c_port_t port = (i2c_port_t)(intptr_t)intf_ptr;
    if (s_i2c_addr == 0) {
        return BME69X_E_COM_FAIL;
    }
    /* 把 reg_addr 当成第 1 个数据字节一起发出 */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (s_i2c_addr << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg_addr, true);
    if (length > 0 && reg_data != NULL) {
        i2c_master_write(cmd, reg_data, length, true);
    }
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(port, cmd, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(cmd);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "i2c_write reg=0x%02x len=%lu failed: %s",
                 reg_addr, (unsigned long)length, esp_err_to_name(err));
        return (int8_t)err;
    }
    return 0;
}

static void bme69x_delay_us(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    if (period < 1000) {
        ets_delay_us(period);
    } else {
        vTaskDelay(pdMS_TO_TICKS((period + 999) / 1000));
    }
}

/* ===== 对外 API ===== */

esp_err_t bme69x_port_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* 1. I2C 总线初始化 (legacy i2c driver) */
    i2c_config_t i2c_conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = BME69X_I2C_SDA_GPIO,
        .scl_io_num       = BME69X_I2C_SCL_GPIO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = BME69X_I2C_FREQ_HZ,
    };
    esp_err_t err = i2c_param_config(I2C_NUM_0, &i2c_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C 初始化: SDA=GPIO%d SCL=GPIO%d Freq=%dHz Addr=0x%02X",
             BME69X_I2C_SDA_GPIO, BME69X_I2C_SCL_GPIO,
             BME69X_I2C_FREQ_HZ, BME69X_I2C_ADDR);

    /* 2. 探一下设备 - 扫描 0x76/0x77（BME690 的 SDO 决定地址:
     *    SDO=GND→0x76, SDO=VDD→0x77。硬件两种都常见, 改用扫描找有效地址,
     *    避免用户因为不知道 SDO 接哪而折腾）。*/
    uint8_t reg = 0xD0;
    uint8_t chip_id = 0;
    uint8_t probed_addr = 0;
    const uint8_t candidate_addrs[2] = { 0x76, 0x77 };
    for (int i = 0; i < 2; ++i) {
        uint8_t addr = candidate_addrs[i];
        chip_id = 0;
        err = i2c_master_write_read_device(I2C_NUM_0, addr,
                                           &reg, 1, &chip_id, 1, pdMS_TO_TICKS(500));
        if (err == ESP_OK && chip_id == BME69X_CHIP_ID) {
            probed_addr = addr;
            break;
        }
        ESP_LOGW(TAG, "I2C 0x%02X 无响应/ID 错 (chip_id=0x%02X, err=%s)",
                 addr, chip_id, esp_err_to_name(err));
    }
    if (probed_addr == 0) {
        ESP_LOGE(TAG, "I2C 扫描 0x76/0x77 均无 BME690 (期望 chip_id 0x61) - 检查接线/上拉/电平/SDO");
        return ESP_FAIL;
    }
    if (probed_addr != BME69X_I2C_ADDR) {
        ESP_LOGW(TAG, "实际 I2C 地址 0x%02X 与配置 0x%02X 不一致, 使用探测值",
                 probed_addr, BME69X_I2C_ADDR);
    } else {
        ESP_LOGI(TAG, "I2C 地址 0x%02X 探测通过", probed_addr);
    }
    /* ⭐ 关键：把探测到的地址写入 s_i2c_addr, bme69x_i2c_read/write
       才能用上正确的地址与 BME690 通信 */
    s_i2c_addr = probed_addr;

    /* 3. 填 Bosch 设备结构 */
    s_dev.intf     = BME69X_I2C_INTF;
    s_dev.read     = bme69x_i2c_read;
    s_dev.write    = bme69x_i2c_write;
    s_dev.delay_us = bme69x_delay_us;
    s_dev.intf_ptr = (void *)(intptr_t)I2C_NUM_0;
    s_dev.amb_temp = 25;  /* 环境温度初始猜测，BSEC/官方都建议 25 */

    /* 4. 调用 Bosch 官方初始化 -> 内部会读全部校准系数 */
    int8_t rslt = bme69x_init(&s_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_init 失败, rslt=%d", rslt);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Bosch BME69x 驱动初始化成功, variant_id=0x%lX",
             (unsigned long)s_dev.variant_id);

    /* 5. 配置 forced 模式参数:
     *    os_hum/os_temp/os_pres = OS_X1 (1 次过采样)
     *    filter = OFF
     *    加热器 200°C / 100ms (BME690 默认就是 GAS 测量必须开 heater, 320°C/150ms
     *    太猛: 紧贴 ESP32-C5 模块时会让 T 自加热 +8~10°C; 200°C/100ms
     *    仍能正常测 gas, 但自加热显著下降, 实测 T 降 3~5°C) */
    struct bme69x_conf conf = {
        .os_hum  = BME69X_OS_1X,
        .os_temp = BME69X_OS_1X,
        .os_pres = BME69X_OS_1X,
        .filter  = BME69X_FILTER_OFF,
        .odr     = BME69X_ODR_NONE,
    };
    rslt = bme69x_set_conf(&conf, &s_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_conf 失败, rslt=%d", rslt);
        return ESP_FAIL;
    }
    struct bme69x_heatr_conf heatr = {
        .enable      = BME69X_ENABLE,
        .heatr_temp  = 200,   /* 原 320 -> 200, 降低自加热 */
        .heatr_dur   = 100,   /* 原 150 -> 100, 缩短加热时间 */
    };
    rslt = bme69x_set_heatr_conf(BME69X_FORCED_MODE, &heatr, &s_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGE(TAG, "bme69x_set_heatr_conf 失败, rslt=%d", rslt);
        return ESP_FAIL;
    }

    s_initialized = true;
    return ESP_OK;
}

esp_err_t bme69x_port_read(float *temperature_c,
                           float *humidity_pct,
                           float *pressure_hpa,
                           float *gas_resistance_ohm)
{
    if (!s_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    /* 触发一次 forced 测量 */
    int8_t rslt = bme69x_set_op_mode(BME69X_FORCED_MODE, &s_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGW(TAG, "set_op_mode(FORCED) failed, rslt=%d", rslt);
        return ESP_FAIL;
    }

    /* 等测量完成 (官方 bme69x_get_meas_dur 返回微秒) */
    struct bme69x_conf conf = {
        .os_hum  = BME69X_OS_1X,
        .os_temp = BME69X_OS_1X,
        .os_pres = BME69X_OS_1X,
        .filter  = BME69X_FILTER_OFF,
        .odr     = BME69X_ODR_NONE,
    };
    uint32_t dur_us = bme69x_get_meas_dur(BME69X_FORCED_MODE, &conf, &s_dev);
    /* 官方推荐: T/P/H 测量时间 + gas heater 时间 + 少量缓冲。
     * heatr.heatr_dur 的单位是 ms；这里当前配置为 100ms。
     * 旧代码误加了 2000000us(2 秒)，会让每次传感器读取阻塞过久，
     * 影响 WiFi/WebSocket 侧的实时性。 */
    uint32_t wait_ms = ((dur_us + 999U) / 1000U) + 100U + 10U;
    vTaskDelay(pdMS_TO_TICKS(wait_ms));

    struct bme69x_data data = {0};
    uint8_t n_data = 0;
    rslt = bme69x_get_data(BME69X_FORCED_MODE, &data, &n_data, &s_dev);
    if (rslt != BME69X_OK) {
        ESP_LOGW(TAG, "bme69x_get_data failed, rslt=%d", rslt);
        return ESP_FAIL;
    }
    if (n_data == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Bosch bme69x.c 在 BME69X_USE_FPU 模式下的内部补偿公式输出单位 (实测):
     *   data.temperature     - 摄氏度 (°C, float, 范围 -40~85)
     *   data.pressure        - 帕斯卡 (Pa, float, 范围 ~90000~110000)
     *                          即 ~900~1100 hPa。bme69x_defs.h 的 MIN/MAX_PRESSURE
     *                          (90000/110000) 印证了这一点。
     *   data.humidity        - 百分比 (%, float, 范围 0~100)
     *   data.gas_resistance  - 欧姆 (Ω, float, BME690 variant 公式)
     *
     * 之前注释错误地说"calc_pressure 内 /100 完毕"是错的! 实际返回 Pa。
     * 真实数据样本 (BME690, 环境~25°C):
     *   T = 38.48°C   (偏高, 是 BME690 紧贴 ESP32 + heater 自加热)
     *   H = 31.93%    (正常, 空调房)
     *   P = 100231.92 -> 100231.92 Pa = 1002.32 hPa (完全正常海平面气压!)
     *
     * 此处把 Pa -> hPa 给上层 (UI/JSON 上报), 同时做范围检查,
     * 越界就当作无效数据不返回 (避免把 I2C 毛刺的 0xFF/0x00 当作真实值)。 */
    float p_pa = data.pressure;
    float p_hpa = p_pa / 100.0f;
    if (temperature_c)        *temperature_c       = data.temperature;
    if (humidity_pct)         *humidity_pct        = data.humidity;
    if (pressure_hpa)         *pressure_hpa        = p_hpa;
    if (gas_resistance_ohm)   *gas_resistance_ohm  = data.gas_resistance;
    /* 范围检查 (真实物理范围):
     *   T  : -40 ~ 85 °C
     *   H  : 0   ~ 100 %
     *   P  : 500 ~ 1100 hPa (覆盖从珠峰到海平面以下)
     *   Gas: 0   ~ 5e6 Ω
     * 越界 = I2C 错误/校准系数错乱, 不让错误值进 JSON。 */
    bool valid = true;
    if (data.temperature < -40.0f || data.temperature > 85.0f) valid = false;
    if (data.humidity    <    0.0f || data.humidity    > 100.0f) valid = false;
    if (p_hpa            <  500.0f || p_hpa            > 1100.0f) valid = false;
    if (data.gas_resistance < 0.0f || data.gas_resistance > 5.0e6f) valid = false;
    if (!valid) {
        ESP_LOGW(TAG, "读数越界, 丢弃: T=%.2f H=%.2f P=%.2fhPa Gas=%.1f",
                 (double)data.temperature, (double)data.humidity,
                 (double)p_hpa, (double)data.gas_resistance);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}
