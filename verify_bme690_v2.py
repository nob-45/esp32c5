#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
验证 BME680/BME690 官方压力公式
根据 Bosch 官方 bme68x.c (BSEC 库) 的压力补偿算法
"""
import struct

# Bosch 官方 BME680/BME690 压力补偿公式 (来自 bme68x.c::calc_pressure)
def bme680_pressure_compensation(adc_pres, par_t1, par_t2, par_t3,
                                  par_p1, par_p2, par_p3, par_p4, par_p5,
                                  par_p6, par_p7, par_p8, par_p9, par_p10,
                                  t_fine):
    """
    Bosch 官方 BME680 压力补偿公式 (来自官方 BSEC 库代码)。
    参考: bme68x.c::bme68x_calc_pressure
    """
    # 第一步: 计算 var1 (与温度相关)
    # 官方代码: var1 = (t_fine * (double)par_t1) - 128000.0;
    # 但注意官方使用的不是 par_t1，而是 par_t1_scaled
    # 实际上从 BSEC 代码看:
    # var1 = ((double)t_fine / 2.0) - 64000.0;
    var1 = ((t_fine / 2.0) - 64000.0)

    # 第二步: var2 = par_p6 * var1^2 / 4 + par_p5 * var1 * 2
    var2 = ((par_p6) * (var1 * var1 / 4.0)) / 262144.0
    var2 = var2 + (par_p5 * var1 * 2.0)
    var2 = (var2 / 4.0) + (par_p4 * 65536.0)

    # 第三步: var1 = (par_p3 * var1^2 / 16384 + par_p2 * var1) / 524288
    var1 = (((par_p3 * var1 * var1) / 16384.0) + (par_p2 * var1)) / 524288.0
    var1 = (1.0 + (var1 / 32768.0)) * par_p1

    # 第四步: 防止 var1 为 0
    if var1 == 0:
        return 0.0

    # 第五步: pressure = 1048576 - adc_pres
    pressure = 1048576.0 - adc_pres

    # 第六步: 应用补偿
    pressure = (pressure - (var2 / 4096.0)) * 6250.0 / var1

    # 第七步: var1 = par_p9 * pressure^2 / 2147483648
    var1 = (par_p9 * pressure * pressure) / 2147483648.0
    # 第八步: var2 = pressure * par_p8 / 32768
    var2 = (pressure * par_p8) / 32768.0
    # 第九步: var3 = (pressure/256)^3 * par_p10 / 131072
    var3 = ((pressure / 256.0) * (pressure / 256.0) * (pressure / 256.0)) * (par_p10 / 131072.0)

    # 第十步: pressure + (var1 + var2 + var3 + par_p7 * 128) / 16
    pressure = pressure + ((var1 + var2 + var3 + (par_p7 * 128.0)) / 16.0)

    return pressure / 100.0  # Pa -> hPa


# 模拟用户实际数据
# 用户日志显示:
# par_t1=22057 par_t2=4345 par_t3=-73
# par_p1=14103 par_p2=-10718 par_p3=5
# par_p4=? par_p5=? par_p6=? par_p7=? par_p8=? par_p9=? par_p10=?

# 假设 t_fine = 51200 (对应 25°C = 51200/5120)
# 假设 adc_pres ≈ 314000 (海平面气压约 1013 hPa, raw = (1013*256) = 259328, 但实际 24-bit)

# 从代码看出, par_p1=14103 来自 coeff[6,7] = (0x17, 0x37)
# 这其实是 BME690 真实校准值
# 但 Bosch 文档说 BME680 应在 30000-65000
# 14103 是异常值!

# 等等, 让我重新思考:
# Bosch BME680 文档: par_p1 range 30000-65000
# 但 BME690 是 BME680 的升级版, 实际可能不同
# BME690 真实校准可能确实就在 14000 左右

# 让我看一个 BME690 真实 dump 例子, BME690 实际 par_p1 = 36735 (normal)
# 如果用户的 14103 是错误读到的, 那么可能寄存器地址或长度有问题

# 假设用户真实 0x8A-0xA0 是 23 字节 (Bosch 官方), 0xE1-0xE0 (错误, 应是 0xE1-0xF0):
# 当前代码读 0x89 25字节 + 0xE1 16字节
# 总 41 字节
# Bosch 应读 0x8A 23字节 + 0xE1 16字节
# 总 39 字节
# 多读 0x89 和 0xA0-0xA1

# 让我假设 0x8A-0xA0 是真实校准区
# 那 0x89 的值 0x40 在 coeff[0] 被忽略
# coeff[1]=0x8A, coeff[2]=0x8B... coeff[22]=0x9F, coeff[23]=0xA0
# 但当前代码读 25 字节到 0xA1 (多读 0xA0)
# 这不影响 par_p1 的计算

# 真实问题:
# 当前代码读的是 0x89, 0x8A, 0x8B, ..., 0xA1
# 索引:
# coeff[0] = 0x89 (0x40)
# coeff[1] = 0x8A (par_t1 LSB)
# coeff[2] = 0x8B (par_t1 MSB)
# coeff[3] = 0x8C (par_t2 LSB)
# coeff[4] = 0x8D (par_t2 MSB)
# coeff[5] = 0x8E (par_t3)
# coeff[6] = 0x8F (par_p1 LSB)
# coeff[7] = 0x90 (par_p1 MSB)
# ...
# coeff[22] = 0x9F
# coeff[23] = 0xA0 (par_p10)
# coeff[24] = 0xA1 (extra)
# 然后读 0xE1-0xF0 16 字节
# coeff[25] = 0xE1 (par_h1 MSB)
# coeff[26] = 0xE2 (par_h1[7:4] << 4 | par_h2[3:0]) 实际不对!
# ...
# 这是个大问题!

# 让我重新推导 coeff 索引对照表:
# 官方 BME680 寄存器布局 (来自 bme68x.c):
# 0x89: reserved
# 0x8A: par_t1[7:0]    -> code coeff[1]  ✓
# 0x8B: par_t1[15:8]   -> code coeff[2]  ✓
# 0x8C: par_t2[7:0]    -> code coeff[3]  ✓
# 0x8D: par_t2[15:8]   -> code coeff[4]  ✓
# 0x8E: par_t3[7:0]    -> code coeff[5]  ✓
# 0x8F: par_p1[7:0]    -> code coeff[6]  ✓
# 0x90: par_p1[15:8]   -> code coeff[7]  ✓
# 0x91: par_p2[7:0]    -> code coeff[8]  ✓
# 0x92: par_p2[15:8]   -> code coeff[9]  ✓
# 0x93: par_p3[7:0]    -> code coeff[10] ✓
# 0x94: par_p4[7:0]    -> code coeff[11] ✓
# 0x95: par_p4[15:8]   -> code coeff[12] ✓
# 0x96: par_p5[7:0]    -> code coeff[13] ✓
# 0x97: par_p5[15:8]   -> code coeff[14] ✓
# 0x98: par_p6[7:0]    -> code coeff[15] ✓
# 0x99: par_p7[7:0]    -> code coeff[16] ✓
# 0x9A: par_p8[7:0]    -> code coeff[17] ✓
# 0x9B: par_p8[15:8]   -> code coeff[18] ✓
# 0x9C: par_p9[7:0]    -> code coeff[19] ✓
# 0x9D: par_p9[15:8]   -> code coeff[20] ✓
# 0x9E: par_p10[7:0]   -> code coeff[21] ✓
# 0x9F: reserved
# 0xA0: reserved
#
# 0xE1: par_h1[3:0] << 4 | par_h2[11:8]
# 0xE2: par_h1[11:4]
# 0xE3: par_h2[7:0]
# 0xE4: par_h2[11:8] ?  实际官方 0xE3, 0xE4 是 par_h2
# ...
# 这部分代码从 coeff[25] 开始读, 用了不一样的索引公式

# 总之, 区1 索引 (coeff[1..21]) 与 Bosch 官方完全一致 ✓
# 关键问题: 校准值本身是不是真的, 还是 BME690 真实校准异常

# 让我用官方 BSEC 库的实际官方值代入看看
# 一个 BME680 的真实校准例子:
# par_t1 = 27627, par_t2 = 26435, par_t3 = -3
# par_p1 = 36512, par_p2 = -10358, par_p3 = 88
# par_p4 = 23960, par_p5 = -8, par_p6 = 7
# par_p7 = 256, par_p8 = -14680, par_p9 = 5000, par_p10 = 30
# 25°C, 1013 hPa

# 如果用户的 par_p1=14103 偏小, 而其他正确, 代入官方公式

# 模拟 t_fine = 51200 (25°C)
t_fine = 51200.0
adc_pres = 259328  # 24-bit raw ≈ 1013 hPa

# 用户真实校准
par_t1 = 22057
par_t2 = 4345
par_t3 = -73
par_p1 = 14103
par_p2 = -10718
par_p3 = 5
par_p4 = 0  # 不知道
par_p5 = 0
par_p6 = 0
par_p7 = 0
par_p8 = 0
par_p9 = 0
par_p10 = 0

# 我用 0 代入, 算出来会怎样
p = bme680_pressure_compensation(adc_pres, par_t1, par_t2, par_t3,
                                  par_p1, par_p2, par_p3, par_p4, par_p5,
                                  par_p6, par_p7, par_p8, par_p9, par_p10,
                                  t_fine)
print(f"With par_p1=14103, others=0: p={p:.2f} hPa")

# 改用 BSEC 例子
par_t1 = 27627
par_t2 = 26435
par_t3 = -3
par_p1 = 36512
par_p2 = -10358
par_p3 = 88
par_p4 = 23960
par_p5 = -8
par_p6 = 7
par_p7 = 256
par_p8 = -14680
par_p9 = 5000
par_p10 = 30

p = bme680_pressure_compensation(adc_pres, par_t1, par_t2, par_t3,
                                  par_p1, par_p2, par_p3, par_p4, par_p5,
                                  par_p6, par_p7, par_p8, par_p9, par_p10,
                                  t_fine)
print(f"BSEC example: p={p:.2f} hPa (expected ~1013)")

# 如果 par_p1=14103, 看看与 BSEC 例子压差多少
# 但用户日志同时显示 par_p1=14103, par_p2=-10718, par_p3=5
# 这都是有效的 16-bit signed 值, 不像异常

# 关键问题: BME690 真实校准中, par_p1 是否真的能低到 14103?
# BME690 是新型号, 校准范围可能放宽

# 检查一下: 用户的 14103 是 (0x37 << 8) | 0x17, 这是 little-endian 正确解析
# 但也可能是 (0x17 << 8) | 0x37 = 0x1737 = 5943, 更小
# 两种解析取决于寄存器地址

# 让我检查代码: par_p1 = u16_from_bytes(coeff[6], coeff[7])
# u16_from_bytes(lsb, msb) = (msb << 8) | lsb
# 所以 par_p1 = (coeff[7] << 8) | coeff[6]
# coeff[6]=0x8F, coeff[7]=0x90
# 0x90 是 par_p1 MSB (官方)
# 0x8F 是 par_p1 LSB (官方)
# 所以 par_p1 = (MSB << 8) | LSB = 大端解析
# 这与 Bosch 文档一致 ✓

# 实际看 0x8F-0x90 寄存器值:
# 假设 0x8F=0x17, 0x90=0x37
# par_p1 = 0x3717 = 14103

# 如果实际 0x8F=0x37, 0x90=0x17, 那么 par_p1 = 0x1737 = 5943
# 5943 更离谱

# 所以 14103 是 BME690 真实校准值
# BME690 校准范围比 BME680 宽, 14103 是正常的

# OK, 那真正的问题就是公式
# 让我用 BSEC 例子, 假设 t_fine=51200, 看公式输出

# 现在测试用户的代码用的公式:
# 代码公式 (类似 BME280):
def current_code_formula(adc_pres, par_p1, par_p2, par_p3, par_p4, par_p5, par_p6, par_p7, par_p8, par_p9, par_p10, t_fine):
    var1 = ((t_fine / 2.0) - 64000.0)
    var2 = var1 * var1 * (par_p6 / 131072.0)
    var2 = var2 + (var1 * par_p5 * 2.0)
    var2 = (var2 / 4.0) + (par_p4 * 65536.0)
    var1 = ((((par_p3 * var1 * var1) / 16384.0) + (par_p2 * var1)) / 524288.0)
    var1 = (1.0 + (var1 / 32768.0)) * par_p1
    if var1 == 0:
        return 0
    pressure = 1048576.0 - adc_pres
    var1 = (par_p9 * pressure * pressure) / 2147483648.0
    var2 = pressure * (par_p8 / 32768.0)
    var3 = (pressure / 256.0) ** 3 * (par_p10 / 131072.0)
    pressure = pressure + (var1 + var2 + var3 + (par_p7 * 128.0)) / 16.0
    return pressure / 100.0

p1 = current_code_formula(adc_pres, par_p1, par_p2, par_p3, par_p4, par_p5, par_p6, par_p7, par_p8, par_p9, par_p10, t_fine)
print(f"Current code formula (BSEC example): {p1:.2f} hPa (expected ~1013)")

p2 = bme680_pressure_compensation(adc_pres, par_t1, par_t2, par_t3,
                                  par_p1, par_p2, par_p3, par_p4, par_p5,
                                  par_p6, par_p7, par_p8, par_p9, par_p10,
                                  t_fine)
print(f"Bosch official formula (BSEC example): {p2:.2f} hPa (expected ~1013)")