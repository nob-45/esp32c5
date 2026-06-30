#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BME690 校准区 1 + 2 真实 16 字节 dump 验证 (扫所有偏移)
"""
import struct

raw = bytes.fromhex("80 00 6F A6 F0 71 F3 F0 4A 59 80 00 00 00 30 00".replace(" ", ""))

print(f"raw ({len(raw)}B):", raw.hex())
print()

# 试所有可能的起始偏移 + 大小端组合
# BME690 应该 = BME680，但先逐个偏移扫一遍

def parse_coeffs(b, off=0):
    """按 BME680 spec 从 offset 处解析"""
    if off + 11 > len(b):
        return None
    par_t1 = b[off+0] | (b[off+1] << 8)
    par_t2 = struct.unpack('<h', b[off+2:off+4])[0]
    par_t3 = struct.unpack('<b', b[off+4:off+5])[0]
    par_p1 = b[off+5] | (b[off+6] << 8)
    par_p2 = struct.unpack('<h', b[off+7:off+9])[0]
    par_p3 = struct.unpack('<b', b[off+9:off+10])[0]
    return {
        "par_t1": par_t1, "par_t2": par_t2, "par_t3": par_t3,
        "par_p1": par_p1, "par_p2": par_p2, "par_p3": par_p3,
    }

# 期望范围 (BME680/690):
EXPECT = {
    "par_t1": (24000, 27000),
    "par_t2": (-2200, -1000),
    "par_t3": (-100, 100),
    "par_p1": (30000, 65000),
    "par_p2": (-10000, 10000),
    "par_p3": (-100, 100),
}

print("=== 扫所有偏移, 看哪个最接近 BME680 期望范围 ===")
for off in range(0, 6):
    c = parse_coeffs(raw, off)
    if c is None: continue
    score = 0
    print(f"\n[偏移 {off}] raw[{off}..{off+10}] = {raw[off:off+11].hex()}")
    for k, v in c.items():
        lo, hi = EXPECT[k]
        ok = lo <= v <= hi
        score += int(ok)
        marker = "✓" if ok else "✗"
        print(f"  {marker} {k}={v} 期望[{lo}..{hi}]")
    print(f"  ==> 命中 {score}/6 个")