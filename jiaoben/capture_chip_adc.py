#!/usr/bin/env python3
"""抓 ESP32 启动后 20 秒的完整输出（包含 BME690 校准 + 测量 + Wi-Fi + 火山 WS）"""
import serial, time, sys, re

PORT = 'COM18'
BAUD = 115200

try:
    s = serial.Serial(PORT, BAUD, timeout=2)
    s.dtr = False; s.rts = False
    time.sleep(0.3)
    s.reset_input_buffer()
    print('=== 串口已开 %s, 等待 ESP32 启动... ===' % PORT, flush=True)

    # 复位 ESP32（拉低 DTR 等同于按 RESET 按钮）
    s.setDTR(False)
    time.sleep(0.1)
    s.setDTR(True)
    time.sleep(0.5)
    s.setDTR(False)
    time.sleep(2)
    s.reset_input_buffer()
    print('=== ESP32 已复位，抓 20 秒数据... ===', flush=True)

    # 抓 20 秒
    data = b''
    start = time.time()
    while time.time() - start < 20:
        chunk = s.read(4096)
        if chunk:
            data += chunk
        sys.stdout.write('.')
        sys.stdout.flush()

    print('\n\n=== 抓取到 %d 字节 ===\n' % len(data), flush=True)

    # 解码
    text = data.decode('utf-8', errors='ignore')
    # 截掉 ANSI escape
    text = re.sub(r'\x1b\[[0-9;]*[a-zA-Z]', '', text)
    # 截掉 wifi 内核噪声
    lines = []
    skip_keys = ['wifi:(opr)', 'wifi:(trc)', 'wifi:(ht)', 'wifi:(he)',
                 'wifi:(extcap)', 'wifi:[ADDBA]', 'wifi:(ba-add)',
                 'wifi:set rx beacon', 'wifi:pm start', 'wifi:AP\'s beacon']
    for line in text.splitlines():
        # 过滤 wifi 噪声
        if any(line.strip().startswith(k) for k in skip_keys):
            continue
        if any(line.strip().startswith('I ('+str(i)+') wifi') for i in range(3000, 8000)):
            continue
        lines.append(line)
    text = '\n'.join(lines)

    print('--- 完整打印（已过滤 wifi 噪声） ---\n', flush=True)
    print(text, flush=True)
    s.close()
except Exception as e:
    print('失败:', e, flush=True)
    sys.exit(1)