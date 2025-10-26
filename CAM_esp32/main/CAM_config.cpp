#include "CAM_sensorRead.hpp"
#include "esp_psram.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_http_server.h"
#include "esp_timer.h"



#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n";




// có hàm bật cam, tắt cam, kiểm tra cam đang bật hay tắt
static const char *TAG = "CAM_CONFIG";
// Khởi tạo camera ESP32-CAM
esp_err_t camera_init(void) {
    camera_config_t config = {};
    
    // Cấu hình chân cho ESP32-CAM 
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = 5;
    config.pin_d1 = 18;
    config.pin_d2 = 19;
    config.pin_d3 = 21;
    config.pin_d4 = 36;
    config.pin_d5 = 39;
    config.pin_d6 = 34;
    config.pin_d7 = 35;
    config.pin_xclk = 0;
    config.pin_pclk = 22;
    config.pin_vsync = 25;
    config.pin_href = 23;
    config.pin_sscb_sda = 26;
    config.pin_sscb_scl = 27;
    config.pin_pwdn = 32;
    config.pin_reset = -1;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    
    // Cấu hình chất lượng ảnh
    if(esp_psram_is_initialized()) {
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
    } else {
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    }

    esp_err_t ret = esp_camera_init(&config);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Camera initialized successfully");
    } else {
        ESP_LOGE(TAG, "Camera init failed with error 0x%x", ret);
    }
    return ret;
}





