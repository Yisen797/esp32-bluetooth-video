"""
OpenMV 持续发帧模式 — 不需要握手
H743 端用 UART4 DMA 接收，解析 [0xAA 0x55] 帧头
"""
import sensor, time
from pyb import UART, LED

FRAMESIZE = sensor.QVGA      # 320x240
UART_BAUD = 921600

uart = UART(3, UART_BAUD, timeout=1000, timeout_char=5)
led = LED(2)

sensor.reset()
sensor.set_pixformat(sensor.JPEG)
sensor.set_framesize(FRAMESIZE)
sensor.set_auto_exposure(True)
sensor.set_auto_gain(True)
sensor.set_auto_whitebal(True)
sensor.skip_frames(time=1500)
led.on()
print("OpenMV continuous stream ready")

while True:
    try:
        img = sensor.snapshot()
        data = img.bytearray()
        n = len(data)
        if not (0 < n <= 65535):
            continue

        uart.write(bytes([0xAA, 0x55, (n >> 8) & 0xFF, n & 0xFF]))

        for i in range(0, n, 256):
            uart.write(data[i:i+256])
            time.sleep_us(200)

        time.sleep_ms(50)  # ~20fps max

    except Exception as e:
        print("skip:", type(e).__name__, e)
        time.sleep_ms(10)
