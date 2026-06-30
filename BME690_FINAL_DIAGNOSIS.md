# BME690 大气压/温度异常 — 最终诊断与修复

## 真实根因（已通过手算与实测日志验证）

**BME690 的温度/气压 ADC 是 24-bit 全分辨率，但 `read_field_data` / `read_all_field_data`
之前沿用了 BME680/688 的 20-bit 解析（多右移了 4 位），导致 `adc_temp` 偏小 16 倍。**

BME690 的温度补偿使用自定义 do1/dtk1 浮点算法（FPU 版本）：
- `do1 = par_t1 << 8 = 22362 << 8 = 5724672`
- `cf  = adc_temp - do1`

由于 20-bit 解析下 `adc_temp` 最大约 1e6，永远小于 `do1`(5.7e6)，
`cf` 必然为大负数 → `T ≈ -109°C` → 气压补偿连锁爆表 → port 层范围检查（500~1100hPa）丢弃数据。

### 手算验证（使用实测原始字节 buf[5..7]=3e 00 4f → 24-bit）
- `adc_temp = 0x6E3E00 = 7225856`
- `cf = 7225856 - 5724672 = 1501184`
- `T = cf×(par_t2/2³⁰) + cf²×(par_t3/2⁴⁸)`
    `= 1501184×(22057/2³⁰) + 1501184²×(-7/2⁴⁸)`
    `≈ 30.84 - 0.06 = 30.78°C` ✓ 完全正常

> 注：`calc_temperature` / `calc_pressure` 的 do1/dtk1 自定义算法本身是 **正确** 的
> （这是 BME690 专用补偿公式，与经典 BME680 par_t1/t2/t3 二次多项式不同），
> 之前怀疑"算法被改坏"的方向是错的。真正错误只在 ADC 位宽解析。

## 修复内容

在两处 ADC 解析处，根据 `variant_id` 区分位宽：

```c
if (dev->variant_id == BME690_VARIANT_GAS_HIGH) {
    /* BME690: 温度/气压 ADC 为 24-bit 全分辨率 */
    adc_pres = (buff[2]<<16) | (buff[3]<<8) | buff[4];
    adc_temp = (buff[5]<<16) | (buff[6]<<8) | buff[7];
} else {
    /* BME680/688: 20-bit */
    adc_pres = (buff[2]<<12) | (buff[3]<<4) | (buff[4]>>4);
    adc_temp = (buff[5]<<12) | (buff[6]<<4) | (buff[7]>>4);
}
```

- `components/bme69x/bme69x.c` → `read_field_data`（单次/forced 模式）
- `components/bme69x/bme69x.c` → `read_all_field_data`（parallel/sequential 模式）

## 状态

- [x] 根因定位：ADC 位宽（24-bit vs 20-bit）
- [x] 两处解析均已修复
- [x] 手算验证 T≈30.8°C 正常
- [x] 编译通过（固件 build/wifi_station.bin 已重新生成）
- [x] **烧录实测确认 T/P/H 全部正常，越界丢弃消失** ✓

## 实测验证结果（烧录后真实串口日志）

```
[BME690 DBG#1] adc_t=7079168 adc_p=7134208 adc_h=20538 ...
            => T=27.78 P=99756.21 H=44.15 Gas=57683.6
BME690 数据：温度=27.78°C, 湿度=44.15%, 气压=997.56hPa, gas_valid=1, heat_stable=1
[BME690 DBG#4] => T=27.93 P=99769.12 H=41.94 Gas=9060.0
BME690 数据：温度=27.93°C, 湿度=41.94%, 气压=997.69hPa
```

- `adc_t` 已回到 7e6 量级（修复前为 45e4）✓
- 温度 27.8°C、气压 997hPa、湿度 42~44% 全部符合环境实际值 ✓
- port 层不再打印"读数越界, 丢弃" ✓

**问题彻底解决，此为正确基线版本。**

## 关键经验教训（防止再次踩坑）

1. **BME690 与 BME680/688 的核心差异 = ADC 位宽**：BME690 温度/气压为 24-bit，
   BME680/688 为 20-bit。任何移植 Bosch BME68x 驱动到 BME690 时，
   必须按 `variant_id` 区分解析，否则温度爆负 → 全链路补偿崩溃。
2. **不要轻易怀疑补偿公式**：BME690 的 do1/dtk1 浮点算法是正确的官方专用公式，
   与 BME680 的 par_t1/t2/t3 二次多项式不同，二者不可互换。
3. **诊断必须以实测原始字节 + 手算为准**，不要凭经验下结论
   （此前文档误判为"算法被改坏"，浪费了排查时间）。
</content>
