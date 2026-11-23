"""
OpenMV 摄像头端 — WiFi 视频发送 (最终稳定版)
============================================================

【硬件】
  OpenMV Cam H7 Plus (STM32H743, 32MB SDRAM, OV5640 传感器)
  固件: OpenMV v4.8.1, MicroPython v1.26.0-77

【接线】
  OpenMV P4 (UART3 TX) ---- ESP32 GPIO16 (RX2)
  OpenMV P5 (UART3 RX) ---- ESP32 GPIO17 (TX2)
  OpenMV GND --------------- ESP32 GND            (务必共地!)

【链路】
  OpenMV --UART3 921600--> ESP32 --WiFi 热点--> 手机/电脑浏览器

【协议 — 逐帧握手】
  ESP32 每收到浏览器 /frame 请求, 就向 OpenMV 发一个字节 'N' 请求一帧。
  OpenMV 收到 'N' 后拍一张 JPEG, 按以下格式发回:
    [0xAA 0x55] [LEN高8位] [LEN低8位] + JPEG数据
  其中 LEN = JPEG 字节数, 大端序, 最大 65535。

【稳定性 — 关键设计决策】
  本项目经历过长时间(2天)的崩溃排查, 最终确认:
  1. OpenMV v4.8.1 固件存在 UART TX DMA 缓存一致性缺陷:
     当单次 `uart.write()` 发送的数据较大时, DMA 写入与 CPU 缓存行冲突,
     导致 STM32H7 硬复位 (表现为控制台 Traceback → 开机 banner)。
     降分辨率/HVGA/降波特率/加 try-except/edit均无法完全避免。
  2. 唯一稳定的工作配置是:
     · sensor.JPEG 模式 (OV5640 硬件 JPEG 编码, 零 CPU 软件参与)
     · QVGA 320×240 (帧约 2~8 KB, 是小帧稳定上限; HVGA/VGA 会崩)
     · 256 字节分块 UART 发送 + 块间 200µs 沉降 (避免大块 DMA)
     · main.py 写入板载 flash 开机会自启 (避免 OpenMV IDE 的 REPL stdin 包装干扰)
  3. sensor.RGB565 + img.compressed() 软件 JPEG 编码也会触发 DMA 冲突, 不可用。
  4. 经典蓝牙方案已放弃: arduino-esp32 2.0.17 的 Bluedroid 栈一收连接即崩
     (btm_sec_connected/L2CAP fixed_queue_new 断言, 属厂商库 bug)。

【参数调优】
  · 分辨率: 改 FRAMESIZE (QVGA 最稳; 可自行试 HVGA/VGA 但可能崩)
  · 亮度暗: sensor.JPEG 模式不支持 gamma_corr, 靠自动曝光 + 浏览器 CSS 提亮
  · 帧率: 取决于 JPEG 帧大小和 921600 波特率, QVGA 通常 8~15 fps
"""

import sensor
import time
from pyb import UART, LED

# ── 可调参数 ──────────────────────────────────────────────
FRAMESIZE = sensor.QVGA   # QVGA 320x240 — 已验证稳定上限 (HVGA/VGA 可能触发 DMA 硬复位)
UART_BAUD = 921600        # 波特率, 必须与 ESP32 端 config.h 中的 OMV_BAUD 一致
# ────────────────────────────────────────────────────────────

# UART3 初始化: timeout=1000ms 读, timeout_char=5ms 字符间隔
uart = UART(3, UART_BAUD, timeout=1000, timeout_char=5)

# 绿灯 (LED2) 常亮 = 脚本正在运行
led = LED(2)

# ── 摄像头初始化 ───────────────────────────────────────────
sensor.reset()
sensor.set_pixformat(sensor.JPEG)   # 【关键】硬件 JPEG: OV5640 传感器直接输出 JPEG
# 此模式零 CPU 软件编码, 绕过 OpenMV v4.8.1 的软件 JPEG DMA 缺陷。
# 代价: 不能做 gamma_corr/lens_corr 等后处理, 但换来绝对稳定。
sensor.set_framesize(FRAMESIZE)
sensor.set_auto_exposure(True)      # 自动曝光/增益/白平衡 — 亮度自适应
sensor.set_auto_gain(True)
sensor.set_auto_whitebal(True)
sensor.skip_frames(time=1500)       # 等待 1.5 秒让自动曝光/白平衡收敛稳定
led.on()                             # 亮灯 = 初始化完成, 进入主循环

print("OpenMV QVGA HW-JPEG ready")

# ── 主循环 — 逐帧握手 ─────────────────────────────────────
while True:
    try:
        # 1) 等待 ESP32 发来的 'N' 请求字节
        c = uart.read(1)
        if not c or c[0] != ord('N'):
            continue

        # 2) 拍摄一帧 (此时 img 已经是一帧 JPEG 编码的图像, 无需 compress)
        img = sensor.snapshot()
        data = img.bytearray()       # 取 JPEG 数据字节
        n = len(data)

        if not (0 < n <= 65535):     # 帧大小异常 (0 或超 16 位上限) 则跳过
            continue

        # 3) 发送帧头: [0xAA 0x55] + 2 字节大端长度
        uart.write(bytes([0xAA, 0x55, (n >> 8) & 0xFF, n & 0xFF]))

        # 4) 发送 JPEG 数据: 切成 256 字节小块, 块间 200µs 沉降
        #    【关键】一次性发大块数据会触发 STM32H7 的 DMA 缓存冲突 → 硬复位。
        #    分块 + 沉降让每块 DMA 传输有足够间隔, 避免与 CPU 缓存行竞争。
        for i in range(0, n, 256):
            uart.write(data[i:i+256])
            time.sleep_us(200)

    except Exception as e:
        # 任何单帧异常都只跳过这帧, 绝不让整个脚本停止
        print("skip:", type(e).__name__, e)
        time.sleep_ms(10)
