#include "CAM_sensorRead.hpp"
#include "CAM_memorFunc.hpp"
#include "CAM_HTTPstream.hpp"
#include "CAM_mqttApi.hpp"
#include "CAM_WiFi.hpp"
#include "CAM_NVS.hpp"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"

static const char* TAG = "MAIN";

// Global managers
static SensorManager* sensorMgr = nullptr;
static SdCardManager* sdCardMgr = nullptr;
static VideoManager* videoMgr = nullptr;
static VideoWriteTimer* videoWriteTimer = nullptr;
static HttpStreamManager* streamMgr = nullptr;
static MqttApiManager* mqttApi = nullptr;
static WiFiConnectionManager* wifiMgr = nullptr;
static httpd_handle_t httpServer = nullptr;

// PIR interrupt task
static TaskHandle_t pirTaskHandle = nullptr;
static volatile bool pirTaskRunning = true;

// HTTP Server handlers
static esp_err_t streamHandler(httpd_req_t* req) {
    return HttpStreamManager::streamHandlerWrapper(req);
}

static esp_err_t initHttpServer() {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.ctrl_port = 32768;
    config.max_open_sockets = 7;
    config.lru_purge_enable = true;
    
    if (httpd_start(&httpServer, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server");
        return ESP_FAIL;
    }
    
    httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = streamHandler,
        .user_ctx = nullptr
    };
    
    httpd_register_uri_handler(httpServer, &stream_uri);
    
    ESP_LOGI(TAG, "HTTP server started on port %u", config.server_port);
    return ESP_OK;
}

// PIR monitoring task
static void pirMonitorTask(void* param) {
    ESP_LOGI(TAG, "PIR monitor task started");
    
    PirSensor& pir = sensorMgr->getPir();
    RtcDS3231& rtc = sensorMgr->getRtc();
    
    bool lastMotionState = false;
    
    while (pirTaskRunning) {
        bool motionDetected = pir.detectMotion();
        
        if (motionDetected && !lastMotionState) {
            ESP_LOGI(TAG, "Motion detected!");
            
            // Check if write is allowed
            if (mqttApi->canWriteVideo()) {
                // Wait for permission (non-blocking check)
                if (mqttApi->waitForWritePermission(100) == ESP_OK) {
                    RtcTime timestamp;
                    if (rtc.readTime(timestamp) == ESP_OK) {
                        // Start or reset write timer
                        if (videoWriteTimer->isActive()) {
                            videoWriteTimer->reset();
                            ESP_LOGI(TAG, "Write timer reset");
                        } else {
                            videoWriteTimer->start(timestamp, 30000, 10); // 30s, 10fps
                            ESP_LOGI(TAG, "Write timer started");
                        }
                    }
                } else {
                    ESP_LOGW(TAG, "Cannot write - resource busy");
                }
            } else {
                ESP_LOGW(TAG, "Write blocked by stream/memory task");
            }
        }
        
        lastMotionState = motionDetected;
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    
    ESP_LOGI(TAG, "PIR monitor task ended");
    vTaskDelete(nullptr);
}

// Cleanup old videos task
static void cleanupTask(void* param) {
    ESP_LOGI(TAG, "Cleanup task started");
    
    RtcDS3231& rtc = sensorMgr->getRtc();
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3600000)); // Every hour
        
        RtcTime currentTime;
        if (rtc.readTime(currentTime) == ESP_OK) {
            ESP_LOGI(TAG, "Running cleanup - deleting videos older than 3 days");
            videoMgr->deleteOldVideos(currentTime, 3);
        }
    }
}

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "=== ESP32-CAM System Starting ===");
    
    // ========== 1. Initialize Sensors ==========
    ESP_LOGI(TAG, "Step 1: Initializing sensors");
    sensorMgr = new SensorManager();
        
        if (sensorMgr->initAll() != ESP_OK) {
            ESP_LOGE(TAG, "Sensor initialization failed!");
            esp_restart();
        }
    
    // ========== 2. Initialize SD Card ==========
        ESP_LOGI(TAG, "Step 2: Initializing SD Card");
        sdCardMgr = new SdCardManager("/sdcard");
    
    if (sdCardMgr->mount() != ESP_OK) {
        ESP_LOGE(TAG, "SD Card mount failed!");
        esp_restart();
    }
    
    // ========== 3. Initialize Video Manager ==========
    ESP_LOGI(TAG, "Step 3: Initializing Video Manager");
    videoMgr = new VideoManager(*sdCardMgr, "/sdcard/videos");
    
    if (videoMgr->init() != ESP_OK) {
        ESP_LOGE(TAG, "Video Manager init failed!");
        esp_restart();
    }
    
    // ========== 4. Initialize WiFi Manager ==========
    ESP_LOGI(TAG, "Step 4: Initializing WiFi Manager");
    wifiMgr = new WiFiConnectionManager();
    
    if (wifiMgr->init() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi Manager init failed!");
        esp_restart();
    }
    
    // WiFi state callback
    wifiMgr->onStateChange([](WiFiState state) {
        switch (state) {
            case WiFiState::CONNECTING:
                ESP_LOGI(TAG, "WiFi: Connecting...");
                break;
            case WiFiState::CONNECTED:
                ESP_LOGI(TAG, "WiFi: Connected!");
                break;
            case WiFiState::DISCONNECTED:
                ESP_LOGW(TAG, "WiFi: Disconnected");
                break;
            case WiFiState::FAILED:
                ESP_LOGE(TAG, "WiFi: Connection failed");
                break;
        }
    });
    
    // Token callback - will be called after WiFi credentials are received
    std::string deviceToken;
    wifiMgr->onTokenReceived([&deviceToken](const std::string& token) {
        ESP_LOGI(TAG, "Device token received: %s", token.c_str());
        deviceToken = token;
    });
    
    // ========== 5. Start WiFi (BLE Provisioning or Direct Connect) ==========
    ESP_LOGI(TAG, "Step 5: Starting WiFi connection");
    if (wifiMgr->start() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed!");
        esp_restart();
    }
    
    // Wait for WiFi connection
    while (!wifiMgr->isConnected()) {
        ESP_LOGI(TAG, "Waiting for WiFi connection...");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    
    ESP_LOGI(TAG, "WiFi connected successfully!");
    
    // ========== 6. Initialize Stream Manager ==========
    ESP_LOGI(TAG, "Step 6: Initializing Stream Manager");
    streamMgr = new HttpStreamManager(sensorMgr->getCamera());
    
    // ========== 7. Initialize MQTT API ==========
    ESP_LOGI(TAG, "Step 7: Initializing MQTT API");
    mqttApi = new MqttApiManager(*streamMgr, *videoMgr, deviceToken);
    
    // Link WiFi manager with MQTT
    wifiMgr->setMqttApi(mqttApi);
    
    if (mqttApi->connect() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT connect failed!");
        esp_restart();
    }
    
    // Wait for MQTT connection
    uint8_t retries = 0;
    while (!mqttApi->isConnected() && retries < 10) {
        ESP_LOGI(TAG, "Waiting for MQTT connection...");
        vTaskDelay(pdMS_TO_TICKS(1000));
        retries++;
    }
    
    if (!mqttApi->isConnected()) {
        ESP_LOGE(TAG, "MQTT connection timeout!");
        esp_restart();
    }
    
    ESP_LOGI(TAG, "MQTT connected successfully!");
    
    // ========== 8. Initialize Video Write Timer ==========
    ESP_LOGI(TAG, "Step 8: Initializing Video Write Timer");
    videoWriteTimer = new VideoWriteTimer(*videoMgr);
    
    // Set callback to publish folder name after video complete
    videoWriteTimer->setOnComplete([](const std::string& folderName) {
        ESP_LOGI(TAG, "Video completed: %s", folderName.c_str());
        mqttApi->publishFolderName(folderName);
    });
    
    // ========== 9. Initialize HTTP Server ==========
    ESP_LOGI(TAG, "Step 9: Initializing HTTP Server");
    if (initHttpServer() != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server init failed!");
        esp_restart();
    }
    
    // ========== 10. Start PIR Monitor Task ==========
    ESP_LOGI(TAG, "Step 10: Starting PIR Monitor Task");
    xTaskCreate(pirMonitorTask, "pir_monitor", 4096, nullptr, 5, &pirTaskHandle);
    
    // ========== 11. Start Cleanup Task ==========
    ESP_LOGI(TAG, "Step 11: Starting Cleanup Task");
    xTaskCreate(cleanupTask, "cleanup", 4096, nullptr, 2, nullptr);
    
    // ========== System Ready ==========
    ESP_LOGI(TAG, "=== System Initialized Successfully ===");
    ESP_LOGI(TAG, "Device Token: %s", deviceToken.c_str());
    ESP_LOGI(TAG, "MQTT Topics:");
    ESP_LOGI(TAG, "  Stream Sub: %s", mqttApi->getTopicStreamSub().c_str());
    ESP_LOGI(TAG, "  Stream Pub: %s", mqttApi->getTopicStreamPub().c_str());
    ESP_LOGI(TAG, "  Memory Sub: %s", mqttApi->getTopicMemorySub().c_str());
    ESP_LOGI(TAG, "  Memory Pub: %s", mqttApi->getTopicMemoryPub().c_str());
    ESP_LOGI(TAG, "  WiFi Pub: %s", mqttApi->getTopicWiFiPub().c_str());
    ESP_LOGI(TAG, "  Folder Name Pub: %s", mqttApi->getTopicSendFolderName().c_str());
    ESP_LOGI(TAG, "HTTP Stream URL: http://<device-ip>/stream");
    
    // ========== Main Loop (Monitor System) ==========
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        
        // Log system status
        ESP_LOGI(TAG, "=== System Status ===");
        ESP_LOGI(TAG, "WiFi: %s", wifiMgr->isConnected() ? "Connected" : "Disconnected");
        ESP_LOGI(TAG, "MQTT: %s", mqttApi->isConnected() ? "Connected" : "Disconnected");
        ESP_LOGI(TAG, "Stream: %s", streamMgr->isActive() ? "Active" : "Idle");
        ESP_LOGI(TAG, "Write Timer: %s", videoWriteTimer->isActive() ? "Active" : "Idle");
        ESP_LOGI(TAG, "Stream State: %d", static_cast<int>(mqttApi->getStreamState()));
        ESP_LOGI(TAG, "Memory State: %d", static_cast<int>(mqttApi->getMemoryState()));
        ESP_LOGI(TAG, "Write State: %d", static_cast<int>(mqttApi->getWriteState()));
        
        // Print SD Card info
        sdCardMgr->printInfo();
        
        // Print video list
        auto videos = videoMgr->listVideos();
        ESP_LOGI(TAG, "Total videos: %zu", videos.size());
    }
}