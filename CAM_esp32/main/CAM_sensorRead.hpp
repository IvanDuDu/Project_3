#ifndef CAM_SENSOR_READ_HPP
#define CAM_SENSOR_READ_HPP

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_camera.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstdint>

// RTC Time structure
struct RtcTime {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    
    RtcTime() : year(2025), month(1), day(1), hour(0), minute(0), second(0) {}
    
    RtcTime(uint16_t y, uint8_t m, uint8_t d, uint8_t h, uint8_t min, uint8_t s)
        : year(y), month(m), day(d), hour(h), minute(min), second(s) {}
};

// PIR Sensor Class
class PirSensor {
private:
    gpio_num_t pin;
    
public:
    explicit PirSensor(gpio_num_t gpio_pin = GPIO_NUM_13);
    ~PirSensor() = default;
    
    esp_err_t init();
    bool detectMotion() const;
};

// RTC (DS3231) Class
class RtcDS3231 {
private:
    i2c_port_t i2c_num;
    gpio_num_t scl_pin;
    gpio_num_t sda_pin;
    uint8_t device_address;
    
    // I2C Master API handles
    i2c_master_bus_handle_t bus_handle;
    i2c_master_dev_handle_t dev_handle;
    
    static uint8_t bcdToDec(uint8_t val);
    static uint8_t decToBcd(uint8_t val);
    
public:
    RtcDS3231(gpio_num_t scl = GPIO_NUM_16, 
              gpio_num_t sda = GPIO_NUM_17,
              i2c_port_t port = I2C_NUM_0);
    ~RtcDS3231();
    
    esp_err_t init();
    esp_err_t readTime(RtcTime& time);
    esp_err_t setTime(const RtcTime& time);
};

// Camera Class
class EspCamera {
private:
    bool initialized;
    camera_config_t config;
    
    void setupDefaultConfig();
    
public:
    EspCamera();
    ~EspCamera();
    
    esp_err_t init();
    camera_fb_t* captureFrame();
    void returnFrameBuffer(camera_fb_t* fb);
    
    // Configuration methods
    void setFrameSize(framesize_t size);
    void setJpegQuality(uint8_t quality);
    void setFrameBufferCount(uint8_t count);
};

// Sensor Manager Class (kết hợp tất cả sensors)
class SensorManager {
private:
    PirSensor pir;
    RtcDS3231 rtc;
    EspCamera camera;
    
    static const char* TAG;
    
public:
    SensorManager();
    ~SensorManager() = default;
    
    // Initialize all sensors
    esp_err_t initAll();
    
    // Getters
    PirSensor& getPir() { return pir; }
    RtcDS3231& getRtc() { return rtc; }
    EspCamera& getCamera() { return camera; }
    
    const PirSensor& getPir() const { return pir; }
    const RtcDS3231& getRtc() const { return rtc; }
    const EspCamera& getCamera() const { return camera; }
};

#endif // CAM_SENSOR_READ_HPP