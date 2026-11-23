/**
 * ESP32 WiFi 视频推流 — OpenMV → ESP32 → 手机/电脑浏览器
 * ============================================================
 *
 * 【硬件】
 *   ESP32-WROOM-32 (无 PSRAM), arduino-esp32 2.0.17
 *
 * 【接线】
 *   ESP32 GPIO16 (RX2) ←── OpenMV P4 (UART3 TX)
 *   ESP32 GPIO17 (TX2) ──→ OpenMV P5 (UART3 RX)
 *   共地务必接好。
 *
 * 【链路】
 *   OpenMV --UART 921600--> ESP32 --WiFi AP--> 浏览器 http://192.168.4.1/
 *
 * 【协议 — 逐帧握手】
 *   ESP32 收到浏览器 /frame HTTP 请求 → 向 OpenMV 发 'N' 请求一帧
 *   → OpenMV 回复: [0xAA 0x55][LEN_HI][LEN_LO] + JPEG数据
 *
 * 【为何不用蓝牙】
 *   ESP32 经典蓝牙 SPP (BluetoothSerial) 在 arduino-esp32 2.0.17 上一收连接
 *   即崩溃。反查 backtrace 确认是官方 Bluedroid 协议栈内部 bug
 *   (btm_sec_connected 空指针 → L2CAP l2cu_allocate_ccb 断言), 已放弃。
 *
 * 【为何用"每帧一个 HTTP 请求"而不是持久 MJPEG 流】
 *   MJPEG 持久流会阻塞 WebServer 单线程, 无法同时服务 /fps 查询和页面刷新。
 *   "抓帧"方案 (每帧一个 /frame GET) 更兼容、可 FPS 自统计、不阻塞。
 *
 * 【OpenMV 稳定性注意事项】
 *   配套的 OpenMV 脚本 (openmv_摄像头端.py) 使用 sensor.JPEG 硬件编码 +
 *   QVGA 320x240 + 256B 分块 UART 发送 —— 这是当前固件 (v4.8.1) 上
 *   唯一验证不崩溃的配置。详细原因见 OpenMV 脚本头注释。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <HardwareSerial.h>

// ═══════════════════════════════════════════════════
//  可调参数
// ═══════════════════════════════════════════════════
#define AP_SSID        "ESP32_VIDEO"   // WiFi 热点名
#define AP_PASS        "12345678"      // 密码 ≥ 8 位
#define OMV_BAUD       921600          // UART 波特率, 必须与 OpenMV 端一致
#define OMV_RX_PIN     16              // ESP32 RX2 ← OpenMV P4 (TX)
#define OMV_TX_PIN     17              // ESP32 TX2 → OpenMV P5 (RX)
#define MAX_FRAME      65535           // 单帧缓冲上限 = 2 字节协议最大值
#define FRAME_TIMEOUT  1500            // 单帧接收总超时 (ms), 超时返回 504

HardwareSerial *omv = &Serial2;        // UART2: 与 OpenMV 通信
WebServer     server(80);              // HTTP 服务器, 端口 80
static uint8_t frame[MAX_FRAME];       // 帧接收缓冲区 (静态分配, 不占堆)

// ═══════════════════════════════════════════════════
//  UART 接收 — 逐帧握手
//  OpenMV 端是分块发送 (256B/块), 本端的 readFull() 会自动
//  循环读取直到收满 len 字节, 因此对分块透明。
// ═══════════════════════════════════════════════════

/** 读 1 字节, 带超时。返回 -1 表示超时。 */
static int readByteTimeout(uint32_t to_ms) {
    uint32_t t = millis();
    while (!omv->available()) {
        if (millis() - t > to_ms) return -1;
        delayMicroseconds(50);         // 让出 CPU, 又不至于太粗粒度
    }
    return omv->read();
}

/** 读满 n 字节到 dst, 超时返回 false。可处理分块到达的数据。 */
static bool readFull(uint8_t *dst, int n, uint32_t to_ms) {
    int got = 0;
    uint32_t t = millis();
    while (got < n) {
        if (omv->available()) {
            got += omv->readBytes(dst + got, n - got);
            t = millis();              // 收到数据则重置超时计时
        } else if (millis() - t > to_ms) {
            return false;
        }
    }
    return true;
}

/**
 * 向 OpenMV 请求一帧并接收。
 * 流程: 清空接收缓冲 → 发 'N' → 等帧头 AA55 → 读长度 → 读数据。
 * 返回 JPEG 数据长度; 0 = 失败/超时。
 */
static int requestFrame() {
    while (omv->available()) omv->read();   // 排空残留, 保证同步干净
    omv->write('N');                        // 请求一帧

    // 找帧头 0xAA 0x55 (允许前面有噪音字节, 自动跳过)
    int prev = 0;
    uint32_t t = millis();
    while (true) {
        int b = readByteTimeout(FRAME_TIMEOUT);
        if (b < 0) return 0;
        if (prev == 0xAA && b == 0x55) break;
        prev = b;
        if (millis() - t > FRAME_TIMEOUT) return 0;
    }

    // 读 2 字节大端长度
    uint8_t lb[2];
    if (!readFull(lb, 2, FRAME_TIMEOUT)) return 0;
    int len = (lb[0] << 8) | lb[1];
    if (len <= 0 || len > MAX_FRAME) return 0;   // 异常长度拒绝

    // 读 JPEG 数据
    if (!readFull(frame, len, FRAME_TIMEOUT)) return 0;
    return len;
}

// ═══════════════════════════════════════════════════
//  网页 — 内嵌 HTML + CSS + JS
//  · 全屏铺满视频, 等比缩放 (object-fit:contain)
//  · image-rendering:pixelated — QVGA 放大也不模糊
//  · 左上角叠加实时 FPS (绿色, 半透明背景)
//  · JS 持续请求 /frame 并替换 <img> 的 src 实现视频播放
// ═══════════════════════════════════════════════════
static const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 视频</title>
<style>
html,body{height:100%;margin:0}
body{background:#111;color:#eee;font-family:sans-serif;display:flex;flex-direction:column}
h3{flex:0 0 auto;margin:0;padding:6px;text-align:center;font-size:15px}
/* 视频容器: flex 自动填充标题下方全部空间 */
.wrap{flex:1 1 auto;min-height:0;position:relative;display:flex;align-items:center;justify-content:center;padding:4px}
/* 画面: 等比填充, 像素锐利缩放 (QVGA 放大不糊), 微调亮度/饱和度 */
img{width:100%;height:100%;object-fit:contain;background:#000;border-radius:6px;
    image-rendering:pixelated;filter:brightness(1.10) saturate(1.08)}
/* FPS 浮层: 左上角, 半透明黑底绿字 */
#fps{position:absolute;top:10px;left:14px;background:rgba(0,0,0,.55);color:#0f8;
     padding:3px 9px;border-radius:6px;font:bold 15px monospace}
</style></head>
<body>
<h3>OpenMV 实时视频</h3>
<div class="wrap">
  <img id="v" alt="等待视频...">
  <div id="fps">FPS: --</div>
</div>
<script>
/* 持续抓帧: 每加载完一张就请求下一张, 浏览器端统计 FPS */
var img=document.getElementById('v'),fpsEl=document.getElementById('fps'),n=0,t=Date.now();
function nextFrame(){var x=new Image();
  /* 加载成功: 替换显示, 计数, 立即请求下一帧 */
  x.onload=function(){img.src=x.src;n++;var now=Date.now();
    /* 每秒更新一次 FPS 显示 */
    if(now-t>=1000){fpsEl.textContent='FPS: '+(n*1000/(now-t)).toFixed(1);n=0;t=now;}nextFrame();};
  /* 加载失败 (如 OpenMV 没响应): 等 300ms 后重试, 不中断流 */
  x.onerror=function(){setTimeout(nextFrame,300);};
  /* cache-busting: 加时间戳参数避免浏览器/ESP32 缓存旧帧 */
  x.src='/frame?'+Date.now();}
nextFrame();
</script>
</body></html>
)HTML";

/** 首页: 返回内嵌 HTML */
static void handleRoot() {
    server.sendHeader("Cache-Control", "no-store");
    server.send_P(200, "text/html", INDEX_HTML);
}

/**
 * 抓帧接口: 向 OpenMV 请求一帧 JPEG, 直接返回给浏览器。
 * 超时/失败返回 HTTP 504, 浏览器 JS 会自动重试。
 */
static void handleFrame() {
    int len = requestFrame();
    if (len <= 0) {
        server.send(504, "text/plain", "no frame");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.setContentLength(len);
    server.send(200, "image/jpeg", "");
    server.client().write(frame, len);       // 将 JPEG 数据写入 HTTP 响应体
}

// ═══════════════════════════════════════════════════
//  启动与主循环
// ═══════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);                     // USB 调试串口
    delay(200);
    Serial.println("\n=== ESP32 WiFi 视频推流 (单端口+FPS) ===");

    // UART2: 与 OpenMV 通信
    omv->setRxBufferSize(MAX_FRAME + 1024);   // 保证收得下最大帧
    omv->begin(OMV_BAUD, SERIAL_8N1, OMV_RX_PIN, OMV_TX_PIN);
    delay(100);
    while (omv->available()) omv->read();     // 排空开机噪音

    // WiFi 热点
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    delay(100);
    Serial.printf("WiFi 热点: %s   密码: %s\n", AP_SSID, AP_PASS);
    Serial.printf("浏览器打开: http://%s/\n", WiFi.softAPIP().toString().c_str());

    // HTTP 路由
    server.on("/", handleRoot);               // 网页
    server.on("/frame", handleFrame);          // JPEG 抓帧
    server.begin();
    Serial.println("HTTP 服务器已启动");
}

void loop() {
    server.handleClient();                    // 处理 HTTP 请求
    delay(1);                                 // 让出 CPU, 喂看门狗
}
