# ESP32-CAM Face Detection & Recognition

基于 ESP-IDF (纯 C，非 Arduino) 的 ESP32-CAM 人脸检测与识别系统，充分利用 ESP32 双核 FreeRTOS 架构。

## 功能

| 功能 | 说明 |
|------|------|
| **MJPEG 实时视频流** | 端口 81，最高 54 FPS |
| **人脸检测** | YCbCr 皮肤模型 + 形态学处理 + 对称性验证 |
| **特征提取** | 128 维灰度直方图 + 梯度特征 |
| **人脸识别** | 余弦相似度匹配（阈值 0.70） |
| **多采样学习** | 同一人多次出现自动加权更新特征 |
| **人脸数据库** | SPIFFS / SD 卡自动切换，FIFO 淘汰旧记录 |
| **Web 控制面板** | 实时视频、检测开关、人脸画廊、图片下载 |
| **SD 卡支持** | SPI 模式自动检测，无卡时回退 SPIFFS |

## 硬件

- **开发板**: Ai-Thinker ESP32-CAM
- **芯片**: ESP32-D0WDQ6（双核 240MHz，4MB Flash，4MB PSRAM）
- **摄像头**: OV2640，QVGA (320×240)，RGB565
- **烧录器**: CH340 USB 转串口，115200 baud

### GPIO 分配

| 功能 | GPIO |
|------|------|
| SD MOSI | 15 |
| SD MISO | 2 |
| SD CLK | 14 |
| SD CS | 13 |

## 软件架构

```
Core 0 (Protocol CPU):
  ├── stream_encoder_task  — 持续从摄像头取帧 → JPEG 编码 → 更新 latest_jpeg
  ├── web_server_task      — TCP :80 接受连接
  │   └── handle_client    — 每连接一个任务，路由 /stats /toggle_detect 等
  └── mjpeg_task           — TCP :81 MJPEG 流，读取 latest_jpeg 缓存

Core 1 (Application CPU):
  └── face_detect_task     — YCbCr 皮肤检测 → blob 分析 → 特征提取 → 匹配/入库
```

### 关键设计

- **双核分离**: 流编码 (Core 0) 与人脸检测 (Core 1) 完全独立，Toggle Detect 不影响视频流
- **JPEG 缓存**: `stream_encoder_task` 持续编码，`mjpeg_task` 通过 `get_latest_jpeg_copy()` 读取缓存副本，无需等待摄像头
- **摄像头**: RGB565 + `fb_count=2` + `CAMERA_GRAB_LATEST`，20MHz XCLK
- **纯 TCP**: 不使用 esp_http_server，所有 HTTP 端点由原生 TCP socket 实现

## 编译与烧录

### 前置条件

- [ESP-IDF v5.4+](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
- Python 3.8+
- USB 线连接 ESP32-CAM（CH340）

### 步骤

```bash
# 1. 克隆仓库
git clone https://github.com/yourusername/esp32cam_face.git
cd esp32cam_face

# 2. 设置 ESP-IDF 环境
# Windows:
export IDF_PATH=/path/to/esp-idf
source export.sh
# 或使用 ESP-IDF PowerShell / ESP-IDF Command Prompt

# 3. 编译
idf.py build

# 4. 烧录（ESP32-CAM 需按住 BOOT 键再烧录）
idf.py -p COM3 flash

# 5. 监控串口输出
idf.py -p COM3 monitor
```

### WiFi 配置

编辑 `main/app_config.h`：

```c
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"
```

烧录后 ESP32 会自动连接 WiFi，串口输出 IP 地址。

## 使用

1. 浏览器打开 `http://<ESP32_IP>`（控制面板）
2. 视频流自动加载（端口 81）
3. **Toggle Detect**: 开关人脸检测（视频不会中断）
4. **Clear All Faces**: 清空人脸数据库
5. **Face Gallery**: 点击人脸缩略图查看详情，点击 **Download Image** 下载

## API 端点

| 端点 | 方法 | 说明 |
|------|------|------|
| `/` | GET | Web 控制面板 HTML |
| `/stats` | GET | JSON 统计（faces, max, sfps, dfps, storage） |
| `/toggle_detect` | GET | 切换检测开关 |
| `/clear_db` | GET | 清空人脸数据库 |
| `/faces_list` | GET | JSON 人脸列表（id, 位置, 时间戳） |
| `/face_image?id=N` | GET | 人脸 JPEG 图片 |

## 分区表

| 分区 | 类型 | 大小 |
|------|------|------|
| nvs | data | 24KB |
| phy_init | data | 4KB |
| factory | app | 1200KB |
| storage | spiffs | 2800KB |

## 存储

- **SPIFFS**: 2800KB，约 186 张 JPEG（受 HARD_MAX_FACES=100 限制）
- **SD 卡**: 按容量自动计算（~12KB/张），最大 100 张
- **FaceRecord**: 544 字节/条（128 个 float + 元数据）
- 每存 5 张人脸自动保存一次数据库

## 限制

- 人脸检测基于颜色和形状，非 AI 模型，可能有误报/漏报
- 识别基于 128 维特征向量的余弦相似度，精度有限
- QVGA 分辨率下小脸检测效果差
- 同时最多检测 8 张脸

## License

MIT
