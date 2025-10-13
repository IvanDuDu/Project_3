#include "CAM_HTTPStream.hpp"
#include "esp_log.h"
#include <cstring>

const char* HttpStreamManager::TAG = "HTTP_STREAM";

// Global instance for static callback
static HttpStreamManager* g_streamMgr = nullptr;

HttpStreamManager::HttpStreamManager(EspCamera& cam)
    : camera(cam), streamTaskHandle(nullptr), isStreaming(false), stopRequested(false) {
    
    mutex = xSemaphoreCreateMutex();
    if (mutex == nullptr) {
        ESP_LOGE(TAG, "Failed to create mutex");
    }
    
    g_streamMgr = this;
}

HttpStreamManager::~HttpStreamManager() {
    stop();
    
    if (mutex != nullptr) {
        vSemaphoreDelete(mutex);
    }
    
    g_streamMgr = nullptr;
}

esp_err_t HttpStreamManager::start() {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (isStreaming) {
        ESP_LOGI(TAG, "Stream already running");
        xSemaphoreGive(mutex);
        return ESP_OK;
    }
    
    stopRequested = false;
    isStreaming = true;
    
    xSemaphoreGive(mutex);
    
    // Tạo stream task
    BaseType_t ret = xTaskCreate(
        streamTaskFunc,
        "http_stream",
        8192,
        this,
        PRIORITY_STREAM_TASK,
        &streamTaskHandle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task");
        
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            isStreaming = false;
            xSemaphoreGive(mutex);
        }
        
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Stream started");
    return ESP_OK;
}

esp_err_t HttpStreamManager::stop() {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (!isStreaming) {
        xSemaphoreGive(mutex);
        return ESP_OK;
    }
    
    stopRequested = true;
    
    xSemaphoreGive(mutex);
    
    // Đợi task kết thúc
    if (streamTaskHandle != nullptr) {
        vTaskDelay(pdMS_TO_TICKS(200));  // Give task time to stop
        
        if (streamTaskHandle != nullptr) {
            vTaskDelete(streamTaskHandle);
            streamTaskHandle = nullptr;
        }
    }
    
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        isStreaming = false;
        stopRequested = false;
        xSemaphoreGive(mutex);
    }
    
    ESP_LOGI(TAG, "Stream stopped");
    return ESP_OK;
}

bool HttpStreamManager::isActive() const {
    bool active = false;
    
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active = isStreaming;
        xSemaphoreGive(mutex);
    }
    
    return active;
}

void HttpStreamManager::streamTaskFunc(void* param) {
    HttpStreamManager* self = static_cast<HttpStreamManager*>(param);
    
    ESP_LOGI(TAG, "Stream task running");
    
    // Task này maintain state, actual streaming được xử lý trong handleStreamRequest
    while (true) {
        bool shouldStop = false;
        
        if (xSemaphoreTake(self->mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            shouldStop = self->stopRequested;
            xSemaphoreGive(self->mutex);
        }
        
        if (shouldStop) {
            ESP_LOGI(TAG, "Stop requested");
            break;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    ESP_LOGI(TAG, "Stream task ended");
    
    // Task tự cleanup
    self->streamTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t HttpStreamManager::handleStreamRequest(httpd_req_t* req) {
    camera_fb_t* fb = nullptr;
    esp_err_t res = ESP_OK;
    char part_buf[64];
    
    ESP_LOGI(TAG, "Handling stream request");
    
    // Set HTTP headers
    res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res != ESP_OK) {
        return res;
    }
    
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    
    // Stream loop
    while (true) {
        // Check stop flag
        bool shouldStop = false;
        if (xSemaphoreTake(mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            shouldStop = stopRequested;
            xSemaphoreGive(mutex);
        }
        
        if (shouldStop) {
            ESP_LOGI(TAG, "Stream stopped by request");
            break;
        }
        
        // Capture frame
        fb = camera.captureFrame();
        if (fb == nullptr) {
            ESP_LOGE(TAG, "Camera capture failed");
            res = ESP_FAIL;
            break;
        }
        
        // Send boundary
        res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
        if (res != ESP_OK) {
            camera.returnFrameBuffer(fb);
            break;
        }
        
        // Send part header
        size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) {
            camera.returnFrameBuffer(fb);
            break;
        }
        
        // Send JPEG data
        res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);
        if (res != ESP_OK) {
            camera.returnFrameBuffer(fb);
            break;
        }
        
        camera.returnFrameBuffer(fb);
        fb = nullptr;
        
        // Frame rate control (~20 fps)
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    
    if (fb != nullptr) {
        camera.returnFrameBuffer(fb);
    }
    
    ESP_LOGI(TAG, "Stream handler ended");
    return res;
}

esp_err_t HttpStreamManager::streamHandlerWrapper(httpd_req_t* req) {
    if (g_streamMgr == nullptr) {
        ESP_LOGE(TAG, "Stream manager not initialized");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    
    return g_streamMgr->handleStreamRequest(req);
}