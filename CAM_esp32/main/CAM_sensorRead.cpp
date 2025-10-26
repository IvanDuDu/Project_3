#include "CAM_sensorRead.hpp"

const char* SensorManager::TAG = "SENSOR_MANAGER";

// ==================== PIR Sensor ====================

PirSensor::PirSensor(gpio_num_t gpio_pin) : pin(gpio_pin) {}

esp_err_t PirSensor::init() {
    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret == ESP_OK) {
        ESP_LOGI("PIR", "Initialized on GPIO %d", pin);
    }
    return ret;
}

bool PirSensor::detectMotion() const {
    return gpio_get_level(pin) == 1;
}

// ==================== RTC DS3231 ====================

RtcDS3231::RtcDS3231(gpio_num_t scl, gpio_num_t sda, i2c_port_t port)
    : i2c_num(port), scl_pin(scl), sda_pin(sda), device_address(0x68), 
      bus_handle(nullptr), dev_handle(nullptr) {}

RtcDS3231::~RtcDS3231() {
    if (dev_handle != nullptr) {
        i2c_master_bus_rm_device(dev_handle);
        dev_handle = nullptr;
    }
    if (bus_handle != nullptr) {
        i2c_del_master_bus(bus_handle);
        bus_handle = nullptr;
    }
}

uint8_t RtcDS3231::bcdToDec(uint8_t val) {
    return (val >> 4) * 10 + (val & 0x0F);
}

uint8_t RtcDS3231::decToBcd(uint8_t val) {
    return ((val / 10) << 4) | (val % 10);
}

esp_err_t RtcDS3231::init() {
    // Configure I2C master bus
    i2c_master_bus_config_t bus_config = {};
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.i2c_port = i2c_num;
    bus_config.scl_io_num = scl_pin;
    bus_config.sda_io_num = sda_pin;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t ret = i2c_new_master_bus(&bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("RTC", "I2C master bus init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Add RTC device to the bus
    i2c_device_config_t dev_config = {};
    dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_config.device_address = device_address;
    dev_config.scl_speed_hz = 100000;

    ret = i2c_master_bus_add_device(bus_handle, &dev_config, &dev_handle);
    if (ret != ESP_OK) {
        ESP_LOGE("RTC", "Failed to add I2C device: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        bus_handle = nullptr;
        return ret;
    }

    ESP_LOGI("RTC", "Initialized (SCL:%d, SDA:%d)", scl_pin, sda_pin);
    return ESP_OK;
}

esp_err_t RtcDS3231::readTime(RtcTime& time) {
    uint8_t reg_addr = 0x00;
    uint8_t data[7];

    // Write register address then read 7 bytes
    esp_err_t ret = i2c_master_transmit_receive(
        dev_handle,
        &reg_addr, 1,
        data, 7,
        1000 / portTICK_PERIOD_MS
    );

    if (ret == ESP_OK) {
        time.second = bcdToDec(data[0] & 0x7F);
        time.minute = bcdToDec(data[1] & 0x7F);
        time.hour = bcdToDec(data[2] & 0x3F);
        time.day = bcdToDec(data[4] & 0x3F);
        time.month = bcdToDec(data[5] & 0x1F);
        time.year = 2000 + bcdToDec(data[6]);
        
        ESP_LOGI("RTC", "%04d-%02d-%02d %02d:%02d:%02d",
                 time.year, time.month, time.day,
                 time.hour, time.minute, time.second);
    } else {
        ESP_LOGE("RTC", "Failed to read time: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t RtcDS3231::setTime(const RtcTime& time) {
    uint8_t write_buf[8];
    write_buf[0] = 0x00;  // Register address
    write_buf[1] = decToBcd(time.second);
    write_buf[2] = decToBcd(time.minute);
    write_buf[3] = decToBcd(time.hour);
    write_buf[4] = 1;  // Day of week (not used)
    write_buf[5] = decToBcd(time.day);
    write_buf[6] = decToBcd(time.month);
    write_buf[7] = decToBcd(time.year - 2000);

    esp_err_t ret = i2c_master_transmit(
        dev_handle,
        write_buf, 8,
        1000 / portTICK_PERIOD_MS
    );

    if (ret == ESP_OK) {
        ESP_LOGI("RTC", "Time set successfully");
    } else {
        ESP_LOGE("RTC", "Failed to set time: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

// ==================== ESP Camera ====================

EspCamera::EspCamera() : initialized(false) {
    setupDefaultConfig();
}

EspCamera::~EspCamera() {
    if (initialized) {
        esp_camera_deinit();
    }
}

void EspCamera::setupDefaultConfig() {
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
    
    // PSRAM check
    #if CONFIG_ESP32_SPIRAM_SUPPORT
        config.frame_size = FRAMESIZE_UXGA;
        config.jpeg_quality = 10;
        config.fb_count = 2;
        config.fb_location = CAMERA_FB_IN_PSRAM;
        config.grab_mode = CAMERA_GRAB_LATEST;
    #else
        config.frame_size = FRAMESIZE_SVGA;
        config.jpeg_quality = 12;
        config.fb_count = 1;
    #endif
}

esp_err_t EspCamera::init() {
    esp_err_t ret = esp_camera_init(&config);
    if (ret == ESP_OK) {
        initialized = true;
        ESP_LOGI("CAMERA", "Initialized successfully");
    } else {
        ESP_LOGE("CAMERA", "Init failed: 0x%x", ret);
    }
    return ret;
}

camera_fb_t* EspCamera::captureFrame() {
    if (!initialized) {
        ESP_LOGE("CAMERA", "Not initialized");
        return nullptr;
    }
    return esp_camera_fb_get();
}

void EspCamera::returnFrameBuffer(camera_fb_t* fb) {
    if (fb != nullptr) {
        esp_camera_fb_return(fb);
    }
}

void EspCamera::setFrameSize(framesize_t size) {
    config.frame_size = size;
}

void EspCamera::setJpegQuality(uint8_t quality) {
    config.jpeg_quality = quality;
}

void EspCamera::setFrameBufferCount(uint8_t count) {
    config.fb_count = count;
}

// ==================== Sensor Manager ====================

SensorManager::SensorManager() 
    : pir(GPIO_NUM_13), rtc(GPIO_NUM_14, GPIO_NUM_15), camera() {}

esp_err_t SensorManager::initAll() {
    ESP_LOGI(TAG, "Initializing all sensors...");
    
    esp_err_t ret = pir.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "PIR init failed");
        return ret;
    }
    
    ret = rtc.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "RTC init failed");
        return ret;
    }
    
    ret = camera.init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed");
        return ret;
    }
    
    ESP_LOGI(TAG, "All sensors initialized successfully");
    return ESP_OK;
}