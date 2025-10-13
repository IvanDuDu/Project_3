#ifndef HTTP_STREAM_HPP
#define HTTP_STREAM_HPP

#include "CAM_sensorRead.hpp"
#include "esp_http_server.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// HTTP Stream Manager Class
class HttpStreamManager {
private:
    EspCamera& camera;
    TaskHandle_t streamTaskHandle;
    SemaphoreHandle_t mutex;
    
    bool isStreaming;
    bool stopRequested;
    
    static const char* TAG;
    static constexpr uint8_t PRIORITY_STREAM_TASK = 6;  // High priority
    
    // MJPEG stream constants
    static constexpr const char* STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=123456789000000000000987654321";
    static constexpr const char* STREAM_BOUNDARY = "\r\n--123456789000000000000987654321\r\n";
    static constexpr const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";
    
    static void streamTaskFunc(void* param);
    
public:
    explicit HttpStreamManager(EspCamera& cam);
    ~HttpStreamManager();
    
    // Disable copy
    HttpStreamManager(const HttpStreamManager&) = delete;
    HttpStreamManager& operator=(const HttpStreamManager&) = delete;
    
    // Main control functions
    esp_err_t start();
    esp_err_t stop();
    bool isActive() const;
    
    // HTTP handler - Được gọi từ HTTP server
    esp_err_t handleStreamRequest(httpd_req_t* req);
    
    // Static wrapper for HTTP server callback
    static esp_err_t streamHandlerWrapper(httpd_req_t* req);
};

#endif // HTTP_STREAM_HPP