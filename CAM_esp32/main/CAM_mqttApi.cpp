#include "CAM_mqttApi.hpp"
#include "esp_log.h"
#include <cstring>

const char* MqttApiManager::TAG = "MQTT_API";

// Global instance for static callback
static MqttApiManager* g_mqttApi = nullptr;

MqttApiManager::MqttApiManager(HttpStreamManager& stream, VideoManager& video, const std::string& token)
    : streamMgr(stream), videoMgr(video), mqttClient(nullptr), mqttConnected(false),
      memoryTaskHandle(nullptr), deviceToken(token),
      streamState(TaskState::IDLE), memoryState(TaskState::IDLE), writeState(TaskState::IDLE) {
    
    // Create topics
    topicStreamSub = "api/" + deviceToken + "/cam/stream";
    topicStreamPub = "api/" + deviceToken + "/cam/stream/status";
    topicMemorySub = "api/" + deviceToken + "/cam/memory";
    topicMemoryPub = "api/" + deviceToken + "/cam/memory/status";
    topicWiFiPub = "api/" + deviceToken + "/cam/connect/status";
    
    // Create event group
    resourceEventGroup = xEventGroupCreate();
    if (resourceEventGroup == nullptr) {
        ESP_LOGE(TAG, "Failed to create event group");
    }
    
    // Create mutex
    resourceMutex = xSemaphoreCreateMutex();
    if (resourceMutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
    
    g_mqttApi = this;
    
    ESP_LOGI(TAG, "MQTT API initialized with token: %s", deviceToken.c_str());
}

MqttApiManager::~MqttApiManager() {
    disconnect();
    
    if (resourceMutex != nullptr) {
        vSemaphoreDelete(resourceMutex);
    }
    
    if (resourceEventGroup != nullptr) {
        vEventGroupDelete(resourceEventGroup);
    }
    
    g_mqttApi = nullptr;
}

esp_err_t MqttApiManager::connect() {
    esp_mqtt_client_config_t mqtt_cfg = {};
    mqtt_cfg.broker.address.uri = MQTT_BROKER_URI;
    mqtt_cfg.credentials.username = MQTT_USERNAME;
    mqtt_cfg.credentials.authentication.password = MQTT_PASSWORD;
    mqtt_cfg.session.keepalive = 60;
    
    mqttClient = esp_mqtt_client_init(&mqtt_cfg);
    if (mqttClient == nullptr) {
        ESP_LOGE(TAG, "Failed to init MQTT client");
        return ESP_FAIL;
    }
    
    esp_mqtt_client_register_event(mqttClient,
                                    static_cast<esp_mqtt_event_id_t>(ESP_EVENT_ANY_ID),
                                    mqttEventHandler, this);
    
    esp_err_t ret = esp_mqtt_client_start(mqttClient);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start MQTT client");
        return ret;
    }
    
    ESP_LOGI(TAG, "MQTT client started");
    return ESP_OK;
}

esp_err_t MqttApiManager::disconnect() {
    if (mqttClient == nullptr) {
        return ESP_OK;
    }
    
    esp_err_t ret = esp_mqtt_client_stop(mqttClient);
    if (ret == ESP_OK) {
        mqttConnected = false;
        esp_mqtt_client_destroy(mqttClient);
        mqttClient = nullptr;
        ESP_LOGI(TAG, "MQTT disconnected");
    }
    
    return ret;
}

void MqttApiManager::mqttEventHandler(void* handler_args, esp_event_base_t base,
                                      int32_t event_id, void* event_data) {
    MqttApiManager* self = static_cast<MqttApiManager*>(handler_args);
    esp_mqtt_event_handle_t event = static_cast<esp_mqtt_event_handle_t>(event_data);
    
    switch (static_cast<esp_mqtt_event_id_t>(event_id)) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT Connected");
            self->mqttConnected = true;
            
            // Subscribe topics
            esp_mqtt_client_subscribe(self->mqttClient, self->topicStreamSub.c_str(), 1);
            ESP_LOGI(TAG, "Subscribed: %s", self->topicStreamSub.c_str());
            
            esp_mqtt_client_subscribe(self->mqttClient, self->topicMemorySub.c_str(), 1);
            ESP_LOGI(TAG, "Subscribed: %s", self->topicMemorySub.c_str());
            
            // Publish initial status
            self->publishStreamStatus(STATUS_OFF);
            break;
            
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "MQTT Disconnected");
            self->mqttConnected = false;
            break;
            
        case MQTT_EVENT_DATA: {
            std::string topic(event->topic, event->topic_len);
            std::string data(event->data, event->data_len);
            
            ESP_LOGI(TAG, "MQTT Data: %s -> %s", topic.c_str(), data.c_str());
            
            if (topic == self->topicStreamSub) {
                self->handleStreamCommand(data);
            } else if (topic == self->topicMemorySub) {
                self->handleMemoryCommand(data);
            }
            break;
        }
            
        case MQTT_EVENT_ERROR:
            ESP_LOGE(TAG, "MQTT Error");
            break;
            
        default:
            break;
    }
}

void MqttApiManager::handleStreamCommand(const std::string& command) {
    ESP_LOGI(TAG, "Stream command: %s", command.c_str());
    
    if (command == CMD_STREAM_ON) {
        if (startStream() != ESP_OK) {
            publishStreamStatus(STATUS_BUSY);
        }
    } else if (command == CMD_STREAM_OFF) {
        stopStream();
    }
}

void MqttApiManager::handleMemoryCommand(const std::string& videoPath) {
    ESP_LOGI(TAG, "Memory command: %s", videoPath.c_str());
    
    if (startMemoryRead(videoPath) != ESP_OK) {
        publishMemoryStatus(STATUS_BUSY);
    }
}

esp_err_t MqttApiManager::startStream() {
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    // Check if memory task is running
    if (memoryState == TaskState::RUNNING) {
        ESP_LOGW(TAG, "Cannot start stream: memory task running");
        xSemaphoreGive(resourceMutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (streamState == TaskState::RUNNING) {
        ESP_LOGI(TAG, "Stream already running");
        xSemaphoreGive(resourceMutex);
        return ESP_OK;
    }
    
    streamState = TaskState::RUNNING;
    xSemaphoreGive(resourceMutex);
    
    // Block write task
    blockWriteVideo();
    
    // Set resource bit
    xEventGroupSetBits(resourceEventGroup, RESOURCE_STREAM_ACTIVE_BIT);
    
    // Start stream
    esp_err_t ret = streamMgr.start();
    
    if (ret == ESP_OK) {
        publishStreamStatus(CMD_STREAM_ON);
        ESP_LOGI(TAG, "Stream started");
    } else {
        if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            streamState = TaskState::IDLE;
            xSemaphoreGive(resourceMutex);
        }
        unblockWriteVideo();
        xEventGroupClearBits(resourceEventGroup, RESOURCE_STREAM_ACTIVE_BIT);
    }
    
    return ret;
}

esp_err_t MqttApiManager::stopStream() {
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (streamState != TaskState::RUNNING) {
        xSemaphoreGive(resourceMutex);
        return ESP_OK;
    }
    
    streamState = TaskState::STOPPING;
    xSemaphoreGive(resourceMutex);
    
    // Stop stream
    streamMgr.stop();
    
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        streamState = TaskState::IDLE;
        xSemaphoreGive(resourceMutex);
    }
    
    // Unblock write task
    unblockWriteVideo();
    
    // Clear resource bit
    xEventGroupClearBits(resourceEventGroup, RESOURCE_STREAM_ACTIVE_BIT);
    
    publishStreamStatus(STATUS_OFF);
    ESP_LOGI(TAG, "Stream stopped");
    
    return ESP_OK;
}

esp_err_t MqttApiManager::startMemoryRead(const std::string& videoPath) {
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    // Check if stream is running
    if (streamState == TaskState::RUNNING) {
        ESP_LOGW(TAG, "Cannot start memory: stream running");
        publishStreamStatus(STATUS_OFF);  // Notify stream interrupted
        xSemaphoreGive(resourceMutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    if (memoryState == TaskState::RUNNING) {
        ESP_LOGW(TAG, "Memory task already running");
        xSemaphoreGive(resourceMutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    memoryState = TaskState::RUNNING;
    memoryVideoPath = videoPath;
    xSemaphoreGive(resourceMutex);
    
    // Block write task
    blockWriteVideo();
    
    // Set resource bit
    xEventGroupSetBits(resourceEventGroup, RESOURCE_MEMORY_ACTIVE_BIT);
    
    // Create memory task
    BaseType_t ret = xTaskCreate(
        memoryTaskFunc,
        "mqtt_memory",
        8192,
        this,
        PRIORITY_MEMORY_TASK,
        &memoryTaskHandle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create memory task");
        
        if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            memoryState = TaskState::IDLE;
            xSemaphoreGive(resourceMutex);
        }
        
        unblockWriteVideo();
        xEventGroupClearBits(resourceEventGroup, RESOURCE_MEMORY_ACTIVE_BIT);
        publishMemoryStatus(STATUS_FAIL);
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Memory task started");
    return ESP_OK;
}

void MqttApiManager::memoryTaskFunc(void* param) {
    MqttApiManager* self = static_cast<MqttApiManager*>(param);
    
    ESP_LOGI(TAG, "Memory task running");
    ESP_LOGI(TAG, "Reading video: %s", self->memoryVideoPath.c_str());
    
    // Read and upload video
    esp_err_t ret = self->videoMgr.readVideo(self->memoryVideoPath);
    
    // Publish result
    if (ret == ESP_OK) {
        self->publishMemoryStatus(STATUS_OK);
        ESP_LOGI(TAG, "Video upload successful");
    } else {
        self->publishMemoryStatus(STATUS_FAIL);
        ESP_LOGE(TAG, "Video upload failed");
    }
    
    // Cleanup
    if (xSemaphoreTake(self->resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        self->memoryState = TaskState::IDLE;
        xSemaphoreGive(self->resourceMutex);
    }
    
    // Unblock write task
    self->unblockWriteVideo();
    
    // Clear resource bit
    xEventGroupClearBits(self->resourceEventGroup, RESOURCE_MEMORY_ACTIVE_BIT);
    
    ESP_LOGI(TAG, "Memory task ended");
    
    self->memoryTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t MqttApiManager::publishStreamStatus(const std::string& status) {
    if (!mqttConnected || mqttClient == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int msg_id = esp_mqtt_client_publish(mqttClient,
                                          topicStreamPub.c_str(),
                                          status.c_str(), 0, 1, 0);
    
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Published stream status: %s", status.c_str());
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

esp_err_t MqttApiManager::publishMemoryStatus(const std::string& status) {
    if (!mqttConnected || mqttClient == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    
    int msg_id = esp_mqtt_client_publish(mqttClient,
                                          topicMemoryPub.c_str(),
                                          status.c_str(), 0, 1, 0);
    
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Published memory status: %s", status.c_str());
        return ESP_OK;
    }
    
    return ESP_FAIL;
}

void MqttApiManager::blockWriteVideo() {
    xEventGroupSetBits(resourceEventGroup, RESOURCE_WRITE_BLOCKED_BIT);
    
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        writeState = TaskState::BLOCKED;
        xSemaphoreGive(resourceMutex);
    }
    
    ESP_LOGI(TAG, "Write video blocked");
}

void MqttApiManager::unblockWriteVideo() {
    xEventGroupClearBits(resourceEventGroup, RESOURCE_WRITE_BLOCKED_BIT);
    
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        writeState = TaskState::IDLE;
        xSemaphoreGive(resourceMutex);
    }
    
    ESP_LOGI(TAG, "Write video unblocked");
}

bool MqttApiManager::canWriteVideo() const {
    EventBits_t bits = xEventGroupGetBits(resourceEventGroup);
    return !(bits & RESOURCE_WRITE_BLOCKED_BIT);
}

esp_err_t MqttApiManager::waitForWritePermission(uint32_t timeoutMs) {
    ESP_LOGI(TAG, "Waiting for write permission...");
    
    TickType_t timeout = (timeoutMs == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
    
    // Wait until RESOURCE_WRITE_BLOCKED_BIT is cleared
    EventBits_t bits;
    TickType_t startTime = xTaskGetTickCount();
    
    while (true) {
        bits = xEventGroupGetBits(resourceEventGroup);
        
        if (!(bits & RESOURCE_WRITE_BLOCKED_BIT)) {
            ESP_LOGI(TAG, "Write permission granted");
            return ESP_OK;
        }
        
        // Check timeout
        if (timeoutMs != 0) {
            TickType_t elapsed = xTaskGetTickCount() - startTime;
            if (elapsed >= timeout) {
                ESP_LOGW(TAG, "Write permission timeout");
                return ESP_ERR_TIMEOUT;
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

TaskState MqttApiManager::getStreamState() const {
    TaskState state = TaskState::IDLE;
    
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = streamState;
        xSemaphoreGive(resourceMutex);
    }
    
    return state;
}

TaskState MqttApiManager::getMemoryState() const {
    TaskState state = TaskState::IDLE;
    
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = memoryState;
        xSemaphoreGive(resourceMutex);
    }
    
    return state;
}

TaskState MqttApiManager::getWriteState() const {
    TaskState state = TaskState::IDLE;
    
    if (xSemaphoreTake(resourceMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        state = writeState;
        xSemaphoreGive(resourceMutex);
    }
    
    return state;
}