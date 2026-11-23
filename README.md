# ESP32 视频传输 (OpenMV → ESP32 → 手机/电脑)

用 ESP32 把 OpenMV 摄像头的 JPEG 视频流通过 **WiFi** 推给手机/电脑浏览器。

```
OpenMV (拍JPEG) --UART 921600--> ESP32 --WiFi 热点--> 浏览器看实时视频
```

## 目录

| 文件 | 说明 |
|------|------|
| `platformio.ini` | ESP32 工程配置 |
| `src/main.cpp` | ESP32 固件: UART 收帧 → WiFi 热点 + HTTP 推流 + 实时 FPS |
| `openmv_摄像头端.py` | OpenMV 端: 逐帧握手, 拍硬件 JPEG 发送 |
| `README.md` | 本文件 |
| `开发记录.md` | 完整开发过程、问题排查、已知限制 |

## 硬件接线

| OpenMV | ESP32 | 说明 |
|--------|-------|------|
| P4 (UART3 TX) | GPIO16 (RX2) | OpenMV 发图, ESP32 收 |
| P5 (UART3 RX) | GPIO17 (TX2) | ESP32 发请求 'N', OpenMV 接收 |
| GND | GND | **务必共地**, 否则串口不通 |

## 使用步骤

### 1. 烧录 ESP32
```bash
cd "<项目根目录>"
pio run -t upload --upload-port COM6
```

### 2. 运行 OpenMV

**方式 A — 开机自启 (推荐)**
将 [openmv_摄像头端.py](openmv_摄像头端.py) 保存为 `main.py` 放入 OpenMV 板载 U 盘,
重新拔插 USB, 自动运行。

**方式 B — IDE 手动运行**
OpenMV IDE 打开 [openmv_摄像头端.py](openmv_摄像头端.py), 点绿色运行。
(不推荐: IDE 的 REPL 包装可能导致间歇性异常)

### 3. 看视频
1. 手机/电脑连 WiFi 热点 **ESP32_VIDEO**, 密码 **12345678**
2. 浏览器打开 **http://192.168.4.1/**
3. 左上角绿色数字 = 实时 FPS

## 配置参数

| 参数 | 默认值 | 位置 |
|------|--------|------|
| WiFi 名/密码 | ESP32_VIDEO / 12345678 | `src/main.cpp` |
| UART 波特率 | 921600 | 两端一致 |
| 分辨率 | QVGA 320×240 (稳定上限) | `openmv_摄像头端.py` FRAMESIZE |
| JPEG 质量 | 传感器默认 (硬件编码) | — |

## 架构说明

- **逐帧握手**: ESP32 发 `'N'` → OpenMV 回一帧 JPEG。帧格式: `[0xAA 0x55][2字节长度]+JPEG数据`
- **HTTP 抓帧**: 浏览器用 JS 不断发 `/frame` 请求获取 JPEG, 避免持久流阻塞服务器
- **FPS**: 浏览器端自统计, 无需服务器参与
- **OpenMV 硬件 JPEG**: `sensor.JPEG` 模式, OV5640 传感器直接编码, 零软件参与

## 已知限制

- 分辨率上限 **QVGA 320×240**: 更高分辨率 (HVGA/VGA) 会触发 OpenMV v4.8.1 固件的 UART DMA 缺陷导致硬复位
- 软件 JPEG 编码 (`img.compress`/`img.compressed`) 不稳定, 只能用传感器硬件 JPEG
- 经典蓝牙方案已放弃 (arduino-esp32 2.0.17 蓝牙栈 bug)
- 详见 [开发记录.md](开发记录.md)
