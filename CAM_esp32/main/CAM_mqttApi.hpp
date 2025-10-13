#ifndef CAM_MQTT_API_HPP
#define CAM_MQTT_API_HPP

#include "CAM_sensorRead.hpp"
#include "CAM_memorFunc.hpp"
#include "CAM_HTTPstream.hpp"
#include "mqtt_client.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include <string>

// Task states
enum class TaskState {
    IDLE = 0,
    RUNNING,
    STOPPING,
    BLOCKED
};

// MQTT API Manager Class
class MqttApiManager {
private:
    // Components
    HttpStreamManager& streamMgr;
    VideoManager& videoMgr;
    
    // MQTT
    esp_mqtt_client_handle_t mqttClient;
    bool mqttConnected;
    
    // Resource management
    EventGroupHandle_t resourceEventGroup;
    SemaphoreHandle_t resourceMutex;
    
    // Task handles
    TaskHandle_t memoryTaskHandle;
    
    // States
    TaskState streamState;
    TaskState memoryState;
    TaskState writeState;
    
    // Device info
    std::string deviceToken;
    std::string topicStreamSub;
    std::string topicStreamPub;
    std::string topicMemorySub;
    std::string topicMemoryPub;
    std::string topicWiFiPub;
    
    // Memory task data
    std::string memoryVideoPath;
    
    static const char* TAG;
    
    // Resource lock bits
    static constexpr int RESOURCE_STREAM_ACTIVE_BIT = (1 << 0);
    static constexpr int RESOURCE_MEMORY_ACTIVE_BIT = (1 << 1);
    static constexpr int RESOURCE_WRITE_BLOCKED_BIT = (1 << 2);
    
    // Task priorities
    static constexpr uint8_t PRIORITY_STREAM_TASK = 6;
    static constexpr uint8_t PRIORITY_MEMORY_TASK = 5;
    static constexpr uint8_t PRIORITY_WRITE_TASK = 3;
    
    // MQTT config
    static constexpr const char* MQTT_BROKER_URI = "mqtt://192.168.1.100:1883";
    static constexpr const char* MQTT_USERNAME = "admin";
    static constexpr const char* MQTT_PASSWORD = "password";
    
    // Commands
    static constexpr const char* CMD_STREAM_ON = "ON";
    static constexpr const char* CMD_STREAM_OFF = "OFF";
    static constexpr const char* STATUS_OK = "ESP_OK";
    static constexpr const char* STATUS_FAIL = "ESP_FAIL";
    static constexpr const char* STATUS_BUSY = "BUSY";
    static constexpr const char* STATUS_OFF = "OFF";
    
    // Internal methods
    void handleStreamCommand(const std::string& command);
    void handleMemoryCommand(const std::string& videoPath);
    
    static void mqttEventHandler(void* handler_args, esp_event_base_t base,
                                 int32_t event_id, void* event_data);
    static void memoryTaskFunc(void* param);
    
public:
    MqttApiManager(HttpStreamManager& stream, VideoManager& video, const std::string& token);
    ~MqttApiManager();
    
    // Disable copy
    MqttApiManager(const MqttApiManager&) = delete;
    MqttApiManager& operator=(const MqttApiManager&) = delete;
    
    // MQTT connection
    esp_err_t connect();
    esp_err_t disconnect();
    bool isConnected() const { return mqttConnected; }
    
    // Stream control (điều khiển từ MQTT)
    esp_err_t startStream();
    esp_err_t stopStream();
    
    // Memory control (điều khiển từ MQTT)
    esp_err_t startMemoryRead(const std::string& videoPath);
    
    // Publish status
    esp_err_t publishStreamStatus(const std::string& status);
    esp_err_t publishMemoryStatus(const std::string& status);
    
    // Write video control
    bool canWriteVideo() const;
    esp_err_t waitForWritePermission(uint32_t timeoutMs = 0);
    void blockWriteVideo();
    void unblockWriteVideo();
    
    // State queries
    TaskState getStreamState() const;
    TaskState getMemoryState() const;
    TaskState getWriteState() const;
};

#endif // CAM_MQTT_API_HPP