#!/usr/bin/env python3
"""模拟 BME680 气压补偿公式，检查不同 par_t1 取值下气压范围"""
import math

# 真实 raw 数据（第一次测量）
# raw[0..15] = 80 00 6F B1 80 72 34 80 4C 7A 80 00 00 00 20 00
# 偏移 0 解析：
adc_pres = (0x80 << 12) | (0x00 << 4) | (0x6F >> 4)  # = 0x80006 = 524294
adc_temp = (0xB1 << 12) | (0x80 << 4) | (0x72 >> 4)  # 修正：之前日志显示 adc_temp=727047
# Wait - 727047 != 上面算的。重新对齐
# 实际 log: adc_temp=727047
# 0xB1 0x80 0x72: 0xB18000 + 0x8000 + 0x70 = 11632640 + 32768 + 112 = 11665520. 不对。
# 0x80 0x72 0x34: (0x80 << 12)|(0x72 << 4)|(0x34>>4) = 0x80000 + 0x720 + 3 = 524291
# log adc_temp=727047: 0xB1 0x80 0x72? = 0xB18000 + 0x7200 + 0x7 = 11634711. 不对。
# 让我反推: 727047 = 0xB1 8xx xxx... = (0xB1<<12) + ... = 11632640 + ...
# 11632640 + 727047 - 11632640 = 727047 - 11632640 + 11632640 = ...
# 727047 = 0xB1807? (hex: 727047 = 0xB1807)
# 0xB1 0x80 0x7? => (0xB1<<12) | (0x80<<4) | (0x70>>4) = 11632640 + 2048 + 7 = 11634695
# 0xB1 0x80 0x47? => 11632640 + 2048 + 4 = 11634692
# 用 0x80 0x72 0x47? => 524288 + 28704 + 2 = 552994
# 727047 = 0xB18 07? => 0xB18070. (0xB1<<12)|(0x80<<4)|(0x70>>4) = 11632640+2048+7 = 11634695
# 实际上 727047 转十六进制: 0xB1807 = 0xB1 * 4096 + 0x80 * 16 + 0x7 = 11632640 + 2048 + 7 = 11634695
# 不对。让我直接算 727047 // 4096 = 177 -> 0xB1 -> 0xB1 0xxx ...
# 727047 / 16 = 45440, /16 = 2840, 727047 mod 16 = 7
# 727047 = 0xB1807 = 0xB1 << 12 | 0x80 << 4 | 0x07
# 所以 data[3]=0xB1, data[4]=0x80, data[5]=0x07
# 但日志打印 raw bytes [0..15]: 80 00 6F B1 80 72 34 80 4C 7A 80 00 00 00 20 00
# 第二个 0x80 0x72 ... 是 press? 不对, 我们固定了 [0..2]=press
# data[0..2]=80 00 6F = adc_pres
# data[3..5]=B1 80 72 -> 0xB18072 >> 4 = 0xB1807 = 727047 ✓

# 实际 log 显示 adc_pres=524294, adc_temp=727047
print(f"adc_pres = {adc_pres} (0x{adc_pres:X})")
adc_temp = 727047
print(f"adc_temp = {adc_temp} (0x{adc_temp:X})")
adc_hum = (0x80 << 8) | 0x00  # data[6..7]
print(f"adc_hum  = {adc_hum} (0x{adc_hum:X})")

# 区 1 真实 raw bytes:
# 40 29 56 F9 10 B7 17 37 10 06 01 58 4F 6F 64 03 FA 00 00 A0 12 03 F5 00 84
# 按 BME680 数据手册 §5.2 寄存器映射（起始 0x89）：
# 0x89 coeff[0]  par_t1[7:0]
# 0x8A coeff[1]  par_t1[15:8]
# ...
C = [0x40, 0x29, 0x56, 0xF9, 0x10, 0xB7, 0x17, 0x37,
     0x10, 0x06, 0x01, 0x58, 0x4F, 0x6F, 0x64, 0x03,
     0xFA, 0x00, 0x00, 0xA0, 0x12, 0x03, 0xF5, 0x00, 0x84]
# 区 2
C += [0x19, 0xE3, 0x0D, 0x32, 0x1E, 0x00, 0x4B, 0x00,
      0x5A, 0x57, 0xC2, 0xEC, 0xD4, 0x12, 0x11, 0x02]

def u16(lo, hi): return (hi << 8) | lo
def s16(lo, hi): v = (hi << 8) | lo; return v - 0x10000 if v >= 0x8000 else v

# BME680 解析（按官方数据手册 §5.2 起始 0x89）
par_t1  = u16(C[0], C[1])
par_t2  = s16(C[2], C[3])
par_t3  = C[4]  # signed
par_p1  = u16(C[5], C[6])
par_p2  = s16(C[7], C[8])
par_p3  = C[9]
par_p4  = s16(C[10], C[11])
par_p5  = s16(C[12], C[13])
par_p6  = C[14]
par_p7  = C[15]
par_p8  = s16(C[16], C[17])
par_p9  = s16(C[18], C[19])
par_p10 = C[20]

print()
print(f"par_t1 = {par_t1}  (BME680 典型 ~27000, 我们 {par_t1})")
print(f"par_t2 = {par_t2}")
print(f"par_t3 = {par_t3}")
print(f"par_p1 = {par_p1}  (BME680 典型 ~36000, 我们 {par_p1})")
print(f"par_p2 = {par_p2}")
print(f"par_p3 = {par_p3}")
print(f"par_p4 = {par_p4}")
print(f"par_p5 = {par_p5}")
print(f"par_p6 = {par_p6}")
print(f"par_p7 = {par_p7}")
print(f"par_p8 = {par_p8}")
print(f"par_p9 = {par_p9}")
print(f"par_p10 = {par_p10}")

# Bosch 浮点公式
def compensate_T(adc_T):
    var1 = (adc_T / 16384.0 - par_t1 / 1024.0) * par_t2
    var2 = (adc_T / 131072.0 - par_t1 / 8192.0) ** 2 * par_t3 * 16.0
    t_fine = var1 + var2
    return t_fine / 5120.0, t_fine

def compensate_P(adc_P, t_fine):
    var1 = t_fine / 2.0 - 64000.0
    var2 = var1 * var1 * par_p6 / 131072.0
    var2 = var2 + var1 * par_p5 * 2.0
    var2 = var2 / 4.0 + par_p4 * 65536.0
    var1 = (par_p3 * var1 * var1 / 16384.0 + par_p2 * var1) / 524288.0
    var1 = (1.0 + var1 / 32768.0) * par_p1
    if int(var1) == 0: return 0.0
    p = 1048576.0 - adc_P
    p = (p - var2 / 4096.0) * 6250.0 / var1
    var1 = par_p9 * p * p / 2147483648.0
    var2 = p * par_p8 / 32768.0
    var3 = (p / 256.0) ** 3 * par_p10 / 131072.0
    p = p + (var1 + var2 + var3 + par_p7 * 128.0) / 16.0
    return p

T, t_fine = compensate_T(adc_temp)
P = compensate_P(adc_pres, t_fine)
print()
print(f"=== 用 BME680 布局（起始 0x89） ===")
print(f"t_fine = {t_fine:.1f}, T = {T:.2f} °C")
print(f"pressure = {P:.2f} Pa = {P/100:.2f} hPa")
print()

# 关键问题: par_t1 = 10560 偏小。是不是 BME690 把 res_heat_range 放在了 0x88，
# 0x89..0xA0 仍是 22 字节？试试只读 22 字节跳过 coeff[0] 当作 reserved
print("=== 假设 BME690 在 0x89 处实际是 res_heat_range，par_t1 应该在 0x8A ===")
# coeff[0]=0x40 是 res_heat_range 0x88 的镜像？试 coeff[0] 跳过
par_t1_alt = u16(C[1], C[2])  # 把 0x29 0x56 当 par_t1
par_t2_alt = s16(C[3], C[4])
par_t3_alt = C[5]
par_p1_alt = u16(C[6], C[7])
par_p2_alt = s16(C[8], C[9])
par_p3_alt = C[10]
par_p4_alt = s16(C[11], C[12])
par_p5_alt = s16(C[13], C[14])
par_p6_alt = C[15]
par_p7_alt = C[16]
par_p8_alt = s16(C[17], C[18])
par_p9_alt = s16(C[19], C[20])
par_p10_alt = C[21]

print(f"par_t1 = {par_t1_alt}  (期望 ~27000, 我们 {par_t1_alt})")
print(f"par_p1 = {par_p1_alt}  (期望 ~36000, 我们 {par_p1_alt})")

# 临时替换系数
par_t1, par_t2, par_t3 = par_t1_alt, par_t2_alt, par_t3_alt
par_p1, par_p2, par_p3 = par_p1_alt, par_p2_alt, par_p3_alt
par_p4, par_p5, par_p6 = par_p4_alt, par_p5_alt, par_p6_alt
par_p7, par_p8, par_p9, par_p10 = par_p7_alt, par_p8_alt, par_p9_alt, par_p10_alt

T2, t_fine2 = compensate_T(adc_temp)
P2 = compensate_P(adc_pres, t_fine2)
print(f"t_fine = {t_fine2:.1f}, T = {T2:.2f} °C")
print(f"pressure = {P2:.2f} Pa = {P2/100:.2f} hPa")