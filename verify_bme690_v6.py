#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
精确模拟：使用用户实测的 par_* 值 + 假设 par_p4..10
反推 BME690 实际 P 是否会变成 2000+ hPa
"""
import struct


def bme690_pres_current_double(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                par_p4, par_p5, par_p6, par_p7, par_p8,
                                par_p9, par_p10):
    """当前代码 double 算法（精确复刻）"""
    var1 = (t_fine / 2.0) - 64000.0
    var2 = var1 * var1 * (par_p6 / 131072.0)
    var2 = var2 + (var1 * par_p5 * 2.0)
    var2 = (var2 / 4.0) + (par_p4 * 65536.0)
    var1 = (((par_p3 * var1 * var1) / 16384.0) + (par_p2 * var1)) / 524288.0
    var1 = (1.0 + (var1 / 32768.0)) * par_p1
    if var1 == 0:
        return 0.0
    pressure = 1048576.0 - pres_adc
    pressure = ((pressure - (var2 / 4096.0)) * 6250.0) / var1
    var1 = (par_p9 * pressure * pressure) / 2147483648.0
    var2 = pressure * (par_p8 / 32768.0)
    var3 = (pressure / 256.0) ** 3 * (par_p10 / 131072.0)
    pressure = pressure + (var1 + var2 + var3 + (par_p7 * 128.0)) / 16.0
    return pressure


def bme690_pres_int32_official(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                par_p4, par_p5, par_p6, par_p7, par_p8,
                                par_p9, par_p10):
    """Bosch 官方 int32 截断算法 (bme68x_dev.c)"""
    # 模拟 C int32 截断
    def i32(x):
        x = int(x)
        return x - (1 << 32) if x >= (1 << 31) else x

    var1 = i32((t_fine >> 1) - 64000)
    var2 = i32(((((var1 >> 2) * (var1 >> 2)) >> 11) * par_p6) >> 2)
    var2 = i32(var2 + ((var1 * par_p5) << 1))
    var2 = i32((var2 >> 2) + (par_p4 << 16))
    var1 = i32(((((var1 >> 2) * (var1 >> 2)) >> 13) * (par_p3 << 5)) >> 3) + \
              i32((par_p2 * var1) >> 1)
    var1 = var1 >> 18
    var1 = i32(((32768 + var1) * par_p1) >> 15)
    pressure = 1048576 - pres_adc
    pressure = i32((pressure - (var2 >> 12)) * 3125)
    if var1 != 0:
        if pressure > 0x7FFFFF:
            pressure = (pressure // var1) << 1
        else:
            pressure = (pressure << 1) // var1
    else:
        return 0
    var1 = (par_p9 * (((pressure >> 3) * (pressure >> 3)) >> 13)) >> 12
    var2 = (pressure >> 2) * (par_p8) >> 13
    var3 = ((pressure >> 8) * (pressure >> 8) * (pressure >> 8) * par_p10) >> 17
    pressure = i32(pressure + ((var1 + var2 + var3 + (par_p7 << 7)) >> 4))
    return pressure


# 用户实测校准值 (部分 + 典型)
t_fine = int(26.49 * 5120)  # 用户日志 T=26.49
par_p1 = 14103; par_p2 = -10718; par_p3 = 5
# 其他 P 系校准值（典型 BME680/690 范围）
# 假设 BME690 真实值
par_p4 = -2438   # BME680 典型
par_p5 = 50      # BME680 典型
par_p6 = -7      # BME680 典型
par_p7 = 800     # BME680 典型
par_p8 = 0       # BME680 典型
par_p9 = -10000  # BME680 典型
par_p10 = 30     # BME680 典型

print(f"t_fine = {t_fine:.0f}")
print(f"par_p1={par_p1} par_p2={par_p2} par_p3={par_p3}")
print(f"par_p4={par_p4} par_p5={par_p5} par_p6={par_p6}")
print(f"par_p7={par_p7} par_p8={par_p8} par_p9={par_p9} par_p10={par_p10}")
print()

# 真实大气压在 100000 Pa 左右，adc_pres 大约 270000-280000
for pres_adc in [250000, 270000, 280000, 290000, 320000]:
    p_d = bme690_pres_current_double(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                      par_p4, par_p5, par_p6, par_p7, par_p8,
                                      par_p9, par_p10)
    p_i = bme690_pres_int32_official(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                      par_p4, par_p5, par_p6, par_p7, par_p8,
                                      par_p9, par_p10)
    print(f"  pres_adc={pres_adc:6d}  double={p_d/100:8.2f} hPa   int32={p_i/100 if p_i>=0 else -1:8.2f} hPa")