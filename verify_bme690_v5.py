#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
对比 Bosch 官方 int32 截断算法 vs 当前 double 算法
"""
import numpy as np


def bme690_pres_official_int(pres_adc, t_fine, par_p1, par_p2, par_p3,
                              par_p4, par_p5, par_p6, par_p7, par_p8,
                              par_p9, par_p10):
    """Bosch 官方 int32 截断算法 (bme68x_dev.c lines 855-895)"""
    var1 = (t_fine >> 1) - 64000
    var2 = ((((var1 >> 2) * (var1 >> 2)) >> 11) * par_p6) >> 2
    var2 = var2 + ((var1 * par_p5) << 1)
    var2 = (var2 >> 2) + (par_p4 << 16)
    var1 = (((((var1 >> 2) * (var1 >> 2)) >> 13) * (par_p3 << 5)) >> 3) + \
           ((par_p2 * var1) >> 1)
    var1 = var1 >> 18
    var1 = ((32768 + var1) * par_p1) >> 15
    pressure = 1048576 - pres_adc
    pressure = (pressure - (var2 >> 12)) * 3125
    if pressure >= (1 << 31) or pressure < 0:
        # overflow
        return -1
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
    pressure = pressure + ((var1 + var2 + var3 + (par_p7 << 7)) >> 4)
    return pressure


def bme690_pres_current_double(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                par_p4, par_p5, par_p6, par_p7, par_p8,
                                par_p9, par_p10):
    """当前代码 double 算法"""
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


# === 测试 BSEC 例子 ===
print("=== BSEC 例子 (T=25°C, 期望 P~101325 Pa) ===")
par_p1 = 36512; par_p2 = -10358; par_p3 = 88
par_p4 = 23960; par_p5 = -8; par_p6 = 7
par_p7 = 256; par_p8 = -14680; par_p9 = 5000; par_p10 = 30
t_fine = 128000  # 25C * 5120
pres_adc = 264129

p_int = bme690_pres_official_int(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                   par_p4, par_p5, par_p6, par_p7, par_p8,
                                   par_p9, par_p10)
p_double = bme690_pres_current_double(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                        par_p4, par_p5, par_p6, par_p7, par_p8,
                                        par_p9, par_p10)
print(f"  Bosch int32 截断算法: P={p_int} Pa = {p_int/100:.2f} hPa")
print(f"  当前 double 算法:     P={p_double:.2f} Pa = {p_double/100:.2f} hPa")
print(f"  期望 (海平面):        ~1013 hPa")
print()


# === 测试用户真实校准值 ===
print("=== 用户真实校准值 (par_p1=14103) ===")
# 用户实测: par_t1=22057, par_t2=4345, par_t3=-73
# par_p1=14103, par_p2=-10718, par_p3=5
# par_p4..10 未知, 假设典型值
# 用户日志 T=26.49, t_fine=26.49*5120=135629
t_fine = int(26.49 * 5120)
par_p1 = 14103; par_p2 = -10718; par_p3 = 5
# 假设其他校准值
par_p4 = -3500; par_p5 = 50; par_p6 = -50
par_p7 = 800; par_p8 = 0; par_p9 = -100; par_p10 = 30
pres_adc = 270000  # 假设

p_int = bme690_pres_official_int(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                   par_p4, par_p5, par_p6, par_p7, par_p8,
                                   par_p9, par_p10)
p_double = bme690_pres_current_double(pres_adc, t_fine, par_p1, par_p2, par_p3,
                                        par_p4, par_p5, par_p6, par_p7, par_p8,
                                        par_p9, par_p10)
print(f"  Bosch int32 截断算法: P={p_int} Pa = {p_int/100:.2f} hPa")
print(f"  当前 double 算法:     P={p_double:.2f} Pa = {p_double/100:.2f} hPa")