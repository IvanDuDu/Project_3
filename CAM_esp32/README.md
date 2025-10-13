# _Điều khiển CAM_

1. CAM_sensorRead.hpp/cpp - Quản lý Sensors
Vai trò: Điều khiển tất cả sensors hardware
Classes:

PirSensor - Cảm biến chuyển động PIR
RtcDS3231 - Đồng hồ thời gian thực RTC
EspCamera - Điều khiển camera ESP32-CAM
SensorManager - Quản lý tập trung tất cả sensors

Struct:

RtcTime - Cấu trúc thời gian chung (dùng xuyên suốt project)

Chức năng chính:
cpp// PIR
bool detectMotion()

// RTC
esp_err_t readTime(RtcTime& time)
esp_err_t setTime(const RtcTime& time)

// Camera
camera_fb_t* captureFrame()
void returnFrameBuffer(camera_fb_t* fb)

// Manager
esp_err_t initAll()  // Init tất cả sensors

2. CAM_memorFunc.hpp/cpp - Quản lý SD Card & Video
Vai trò: Lưu trữ và quản lý video trên SD Card
Classes:

SdCardManager - Quản lý SD Card (mount/unmount/info)
VideoManager - Ghi/Đọc/Xóa video
VideoWriteTimer - Timer tự động cho write video (PIR-triggered)

Namespace:

RtcTimeUtils - Extension functions cho RtcTime

Chức năng chính:
cpp// SD Card
esp_err_t mount()
esp_err_t getInfo(uint64_t& total, uint64_t& free)

// Video Manager
esp_err_t writeVideo(const RtcTime& timestamp, uint32_t duration, uint8_t fps, VideoInfo& info)
esp_err_t readVideo(const std::string& folderPath)  // Upload qua HTTP
esp_err_t deleteOldVideos(const RtcTime& current, uint32_t daysOld)

// Write Timer
esp_err_t start(const RtcTime& timestamp, uint32_t duration, uint8_t fps)
esp_err_t reset()  // Reset timer (kéo dài 10s)
esp_err_t stop()
Cấu trúc lưu trữ:
/sdcard/videos/
├── 20250110120530/    (YYYYMMDDHHmmss)
│   ├── 0001.jpg
│   ├── 0002.jpg
│   └── ...
└── 20250110143022/
    └── ...

3. HTTPStream.hpp/cpp - HTTP MJPEG Streaming
Vai trò: Stream video realtime qua HTTP
Classes:

HttpStreamManager - Quản lý MJPEG stream

Chức năng chính:
cppesp_err_t start()  // Bắt đầu stream
esp_err_t stop()   // Dừng stream
esp_err_t handleStreamRequest(httpd_req_t* req)  // Handle HTTP request
Endpoint:

http://[ESP32_IP]/stream - MJPEG stream

Format: Multipart/x-mixed-replace (MJPEG)

4. CAM_mqttApi.hpp/cpp - MQTT API Controller
Vai trò: Điều khiển Stream và Memory Upload qua MQTT
Classes:

MqttApiManager - Quản lý MQTT và resource locking

Enum:

TaskState - IDLE, RUNNING, STOPPING, BLOCKED

Chức năng chính:
cpp// MQTT
esp_err_t connect()
esp_err_t publishStreamStatus(const std::string& status)
esp_err_t publishMemoryStatus(const std::string& status)

// Stream control (từ MQTT)
esp_err_t startStream()
esp_err_t stopStream()

// Memory control (từ MQTT)
esp_err_t startMemoryRead(const std::string& videoPath)

// Write video control
bool canWriteVideo()
esp_err_t waitForWritePermission(uint32_t timeout)
void blockWriteVideo()
void unblockWriteVideo()
MQTT Topics:
Subscribe:
- api/{token}/cam/stream     → Nhận ON/OFF
- api/{token}/cam/memory     → Nhận video_path

Publish:
- api/{token}/cam/stream/status  → Gửi ON/OFF/BUSY
- api/{token}/cam/memory/status  → Gửi ESP_OK/ESP_FAIL/BUSY

5. CAM_WiFi_NVS.hpp/cpp - WiFi & BLE Provisioning
Vai trò: Kết nối WiFi, lưu credentials, BLE provisioning
Classes:

NvsManager - Lưu/Đọc credentials từ NVS
BleProvisioningManager - BLE provisioning (pair với mobile app)
WiFiConnectionManager - Quản lý WiFi connection

Chức năng chính:
cpp// NVS
esp_err_t saveCredentials(const WiFiCredentials& creds)
esp_err_t loadCredentials(WiFiCredentials& creds)
bool hasCredentials()

// BLE Provisioning
esp_err_t start()  // Bật BLE advertising
esp_err_t sendAck()  // Gửi ACK về mobile app

// WiFi Manager
esp_err_t start()  // Auto: Load NVS → Connect WiFi HOẶC BLE provisioning
WiFiState getState()
Logic WiFi:

Check NVS có credentials không?

YES → Connect WiFi
NO → Bật BLE provisioning


BLE nhận: <ssid>//<password>//<token>
Parse và lưu vào NVS
Connect WiFi
Gửi ACK về mobile app qua BLE
Publish "active" qua MQTT
Monitor WiFi → Nếu mất kết nối → Publish "inactive"


6. main.cpp - Entry Point
Vai trò: Khởi tạo và điều phối tất cả components
Sequence khởi tạo:

Init WiFi (NVS + BLE provisioning nếu cần)
Init Sensors (PIR, RTC, Camera)
Init SD Card
Init Video Manager
Init Write Timer
Init Stream Manager
Init HTTP Server
Init MQTT API
Create tasks (PIR monitor, System monitor)


🔄 Luồng hoạt động (Flow Diagram)
Flow 1: PIR → Write Video với Timer
┌──────────────┐
│  PIR Detect  │
└──────┬───────┘
       │
       ▼
┌─────────────────────────────────┐
│ Check canWriteVideo()?          │
│ (Stream/Memory không chạy)      │
└──────┬──────────────────────────┘
       │
       ├─ NO → Wait or Skip
       │
       └─ YES
          │
          ▼
    ┌────────────────────────┐
    │ writeTimer.isActive()? │
    └──────┬─────────────────┘
           │
           ├─ YES → Reset Timer (kéo dài 10s)
           │
           └─ NO → Start Timer (10s) + Start Write
                   │
                   ▼
              ┌─────────────────┐
              │ Write Video     │
              │ (frames → SD)   │
              └─────────────────┘
                   │
                   ▼
              ┌─────────────────┐
              │ Timeout 10s     │
              │ (No PIR)        │
              └─────────────────┘
                   │
                   ▼
              ┌─────────────────┐
              │ Stop Write      │
              └─────────────────┘

Flow 2: MQTT Stream Control
┌─────────────────────────────┐
│ MQTT Broker                 │
│ Publish "ON" →              │
│ api/{token}/cam/stream      │
└──────────┬──────────────────┘
           │
           ▼
┌──────────────────────────────┐
│ MqttApiManager receives      │
└──────────┬───────────────────┘
           │
           ▼
┌──────────────────────────────┐
│ Check: Memory task running?  │
└──────────┬───────────────────┘
           │
           ├─ YES → Reply "BUSY"
           │
           └─ NO
              │
              ▼
         ┌──────────────────┐
         │ Block Write Task │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────┐
         │ streamMgr.start()│
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────────┐
         │ Publish "ON" status  │
         └────────┬─────────────┘
                  │
                  ▼
         ┌──────────────────────┐
         │ Client access        │
         │ http://[IP]/stream   │
         └────────┬─────────────┘
                  │
                  ▼
         ┌──────────────────────┐
         │ MJPEG Streaming...   │
         └──────────────────────┘

Flow 3: MQTT Memory Upload
┌─────────────────────────────────────┐
│ MQTT Broker                         │
│ Publish "/sdcard/videos/20250110.." │
│ → api/{token}/cam/memory            │
└──────────┬──────────────────────────┘
           │
           ▼
┌──────────────────────────────────────┐
│ MqttApiManager receives              │
└──────────┬───────────────────────────┘
           │
           ▼
┌──────────────────────────────────────┐
│ Check: Stream running?               │
└──────────┬───────────────────────────┘
           │
           ├─ YES → Publish "OFF" to stream
           │         Reply "BUSY"
           │
           └─ NO
              │
              ▼
         ┌──────────────────┐
         │ Block Write Task │
         └────────┬─────────┘
                  │
                  ▼
         ┌──────────────────────────┐
         │ Create Memory Task (P5)  │
         └────────┬─────────────────┘
                  │
                  ▼
         ┌──────────────────────────┐
         │ Read folder              │
         │ Get all .jpg files       │
         └────────┬─────────────────┘
                  │
                  ▼
         ┌──────────────────────────┐
         │ For each file:           │
         │ - Read from SD           │
         │ - HTTP POST to server    │
         └────────┬─────────────────┘
                  │
                  ▼
         ┌──────────────────────────┐
         │ Publish result           │
         │ (ESP_OK / ESP_FAIL)      │
         └────────┬─────────────────┘
                  │
                  ▼
         ┌──────────────────────────┐
         │ Unblock Write Task       │
         └──────────────────────────┘

Flow 4: WiFi Provisioning via BLE
┌──────────────────┐
│ Power ON         │
└────────┬─────────┘
         │
         ▼
┌──────────────────────────┐
│ Check NVS                │
│ Has credentials?         │
└────────┬─────────────────┘
         │
         ├─ YES → Connect WiFi → Done
         │
         └─ NO
            │
            ▼
       ┌──────────────────┐
       │ Start BLE        │
       │ Advertising      │
       └────────┬─────────┘
                │
                ▼
       ┌──────────────────────┐
       │ Mobile App connects  │
       │ via BLE              │
       └────────┬─────────────┘
                │
                ▼
       ┌──────────────────────────────┐
       │ Mobile sends:                │
       │ "<ssid>//<pass>//<token>"    │
       └────────┬─────────────────────┘
                │
                ▼
       ┌──────────────────────┐
       │ Parse credentials    │
       └────────┬─────────────┘
                │
                ▼
       ┌──────────────────────┐
       │ Save to NVS          │
       └────────┬─────────────┘
                │
                ▼
       ┌──────────────────────┐
       │ Connect WiFi         │
       └────────┬─────────────┘
                │
                ▼
       ┌──────────────────────┐
       │ Send ACK via BLE     │
       └────────┬─────────────┘
                │
                ▼
       ┌──────────────────────┐
       │ Stop BLE             │
       └────────┬─────────────┘
                │
                ▼
       ┌──────────────────────────┐
       │ Publish "active"         │
       │ → api/{token}/activity/  │
       └──────────────────────────┘


┌────────────────────────────────────────────────────────────────┐
│                         MQTT Broker                            │
│  api/{token}/cam/stream  ←→  Stream Control                   │
│  api/{token}/cam/memory  ←→  Memory Upload                    │
│  api/{token}/activity/   ←→  WiFi Status (active/inactive)    │
└──────────────────────────┬─────────────────────────────────────┘
                           │
                           ▼
┌───────────────────────────────────────────────────────────────┐
│                        ESP32-CAM                              │
│                                                               │
│  ┌─────────────────────────────────────────────────────────┐ │
│  │              WiFiConnectionManager                      │ │
│  │  - Auto provisioning (NVS / BLE)                       │ │
│  │  - Monitor WiFi → Publish active/inactive             │ │
│  └─────────────────────────────────────────────────────────┘ │
│                           │                                   │
│  ┌────────────────────────┴────────────────────────────────┐ │
│  │              MqttApiManager                             │ │
│  │  - Subscribe stream/memory topics                      │ │
│  │  - Resource locking (Event Groups)                     │ │
│  │  - Publish status                                      │ │
│  └─────┬──────────────────┬───────────────────────────────┘ │
│        │                  │                                  │
│        ▼                  ▼                                  │
│  ┌──────────────┐   ┌──────────────┐                       │
│  │ HttpStream   │   │ VideoManager │                       │
│  │ Priority: 6  │   │ + HTTP POST  │                       │
│  │ (Highest)    │   │ Priority: 5  │                       │
│  └──────────────┘   └──────────────┘                       │
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │         PIR Monitor Task (Priority: 3)                  ││
│  │  PIR detect → Check canWriteVideo() → VideoWriteTimer  ││
│  │                                                         ││
│  │  ┌───────────────────────────────────────────────┐    ││
│  │  │  VideoWriteTimer (10s timeout)                │    ││
│  │  │  - PIR trigger → Start/Reset timer           │    ││
│  │  │  - Write video to SD Card                    │    ││
│  │  │  - Timeout → Stop write                      │    ││
│  │  └───────────────────────────────────────────────┘    ││
│  └─────────────────────────────────────────────────────────┘│
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │             SensorManager                               ││
│  │  - PIR Sensor   - RTC DS3231   - ESP Camera            ││
│  └─────────────────────────────────────────────────────────┘│
│                                                              │
│  ┌─────────────────────────────────────────────────────────┐│
│  │             SdCardManager                               ││
│  │  - Mount/Unmount   - Get Info   - File Operations      ││
│  └─────────────────────────────────────────────────────────┘│
└───────────────────────────────────────────────────────────────┘

## Cấu trúc files:
```
esp32_cam_cpp_project/
├── CMakeLists.txt                 (Project root)
├── main/
│   ├── CMakeLists.txt            (Component)
│   ├── main.cpp                  (Entry point)
│   ├── CAM_sensorRead.hpp        (Sensors: PIR, RTC, Camera)
│   ├── CAM_sensorRead.cpp
│   ├── CAM_memorFunc.hpp         (SD Card, Video Manager, Write Timer)
│   ├── CAM_memorFunc.cpp
│   ├── HTTPStream.hpp            (HTTP Stream Manager)
│   ├── HTTPStream.cpp
│   ├── CAM_mqttApi.hpp           (MQTT API Manager)
│   ├── CAM_mqttApi.cpp
|   ├── CAM_WiFi_NVS.cpp
|   └── CAM_WiFI_NVS.hpp
|
└── components/
    └── esp32-camera/             (Camera driver)
```
Additionally, the sample project contains Makefile and component.mk files, used for the legacy Make based build system. 
They are not used or needed when building with CMake and idf.py.
