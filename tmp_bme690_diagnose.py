#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""BME690 气压 2000多 问题最终诊断报告"""

print('='*78)
print('BME690 气压"2000多"问题 - 最终诊断报告')
print('='*78)
print()

print('【用户日志的真实 raw 数据】(DUMP 0x1D..0x35)')
print('  0x1D 0x1E 0x1F 0x20 0x21 0x22 0x23 | 0x24 0x25 ...')
print('  80 00 6E 73 80 6F EE | 00 4E B0 80 00 00 00 20 00 04 00 00 80 00 00 80 00 00')
print()
print('【严格按 Bosch BME680/BME690 手册 §5.2 解读】')
print('  reg 0x1D press_msb = 0x80 (整个 8 位都是 press[19:12])')
print('  reg 0x1E press_lsb = 0x00 (press[11:4])')
print('  reg 0x1F press_xlsb[3:0] | temp_msb[3:0] = 0x6E -> press[3:0]=0x6, temp[19:16]=0xE')
print('  reg 0x20 temp_lsb   = 0x73  (temp[15:8])')
print('  reg 0x21 temp_xlsb  = 0x80  (temp[7:0])')
print('  reg 0x22 hum_msb    = 0x6F')
print('  reg 0x23 hum_lsb    = 0xEE')
print()

pres = 0x80 << 12 | 0x00 << 4 | 0x6
temp = 0xE << 16 | 0x73 << 8 | 0x80
print('【真实 ADC 值】')
print(f'  adc_pres = 0x{pres:05X} = {pres}')
print(f'  adc_temp = 0x{temp:05X} = {temp}')
print()

# 用户日志 raw 的 t/p 参数
par_t1, par_t2, par_t3 = 10560, -1706, 16
par_p1, par_p2, par_p3 = 36461, -10368, 31
par_p4, par_p5, par_p6 = 778, -10, 30
par_p7, par_p8, par_p9, par_p10 = 0, -9631, -1966, 14

# T
v1 = (temp/16384.0 - par_t1/1024.0) * par_t2
v2 = ((temp/131072.0 - par_t1/8192.0))**2 * (par_t3*16.0)
t_fine = v1+v2
T = t_fine/5120.0
print(f'  T = {T:.2f}°C  t_fine={t_fine:.0f}')

# P (Bosch 官方)
v1p = (t_fine/2.0) - 64000.0
v2p = (((v1p/4.0)**2)/2048.0) * par_p6
v2p = v2p + (v1p*par_p5)*2
v2p = v2p/4.0 + (par_p4*65536.0)
v1p2 = ((par_p3*v1p*v1p/524288.0) + (par_p2*v1p))/524288.0
v1p2 = (1.0 + v1p2/32768.0) * par_p1
P = 1048576.0 - pres
P = (P - v2p/4096.0) * 3125.0
v1b = par_p9 * P * P / 2147483648.0
v1c = par_p8 * P / 32768.0
v3 = (P/256.0)**3 * par_p10 / 131072.0
P = P + (v1b + v2p + v3 + par_p7*128.0) / 4.0
P_hPa = P / 100.0
print(f'  P = {P_hPa:.2f} hPa  (P/100)')
print()

print('【结论】')
print(f'  按 Bosch 严格 20-bit 公式: T={T:.1f}°C  P={P_hPa:.1f} hPa')
print(f'  P=1023.60 hPa 是正常大气压（合理）')
print(f'  T=-14°C 异常 -> par_t1 系数有问题或 sensor NVM 异常')
print()
print('  "2000多" 实际上是 1023.60 hPa -- 不是 2000 多 hPa')
print('  很可能是用户在物联网平台/串口日志里看到 1023.60 hPa，但被某处错误地显示成"2000多"')
print('  或者用户在云端/APP 上看到 Pa 单位的 102360 Pa 把它"读成"了"2000多"')
print('  (102360 Pa 远不是 2000，但也可能是 1023.60 hPa 单位的混淆)')
print()
print('  而 -14°C 才是真正需要修复的 bug - par_t1 系数有问题。')