#include "CAM_memorFunc.hpp"
#include "CAM_sensorRead.hpp"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include <sys/stat.h>
#include <dirent.h>
#include <cstring>
#include <ctime>

const char* SdCardManager::TAG = "SD_CARD";
const char* VideoManager::TAG = "VIDEO_MGR";
const char* VideoWriteTimer::TAG = "WRITE_TIMER";

// ==================== RtcTime Utils ====================

namespace RtcTimeUtils {
    std::string toFolderName(const RtcTime& time) {
        char buf[15];
        snprintf(buf, sizeof(buf), "%04d%02d%02d%02d%02d%02d",
                 time.year, time.month, time.day, 
                 time.hour, time.minute, time.second);
        return std::string(buf);
    }

    RtcTime daysAgo(const RtcTime& time, uint32_t days) {
        struct tm timeinfo = {};
        timeinfo.tm_year = time.year - 1900;
        timeinfo.tm_mon = time.month - 1;
        timeinfo.tm_mday = time.day;
        timeinfo.tm_hour = time.hour;
        timeinfo.tm_min = time.minute;
        timeinfo.tm_sec = time.second;
        
        time_t timestamp = mktime(&timeinfo);
        timestamp -= days * 24 * 60 * 60;
        
        struct tm* new_time = localtime(&timestamp);
        return RtcTime(
            new_time->tm_year + 1900,
            new_time->tm_mon + 1,
            new_time->tm_mday,
            new_time->tm_hour,
            new_time->tm_min,
            new_time->tm_sec
        );
    }
    
    bool isOlderThan(const RtcTime& t1, const RtcTime& t2) {
        if (t1.year != t2.year) return t1.year < t2.year;
        if (t1.month != t2.month) return t1.month < t2.month;
        if (t1.day != t2.day) return t1.day < t2.day;
        if (t1.hour != t2.hour) return t1.hour < t2.hour;
        if (t1.minute != t2.minute) return t1.minute < t2.minute;
        return t1.second < t2.second;
    }
    
    bool isSameDay(const RtcTime& t1, const RtcTime& t2) {
        return t1.year == t2.year && t1.month == t2.month && t1.day == t2.day;
    }
}

// ==================== SD Card Manager ====================

SdCardManager::SdCardManager(const std::string& mount_point)
    : card(nullptr), mounted(false), mountPoint(mount_point) {}

SdCardManager::~SdCardManager() {
    unmount();
}

esp_err_t SdCardManager::mount() {
    if (mounted) {
        ESP_LOGW(TAG, "Already mounted");
        return ESP_OK;
    }
    
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 10,
        .allocation_unit_size = 16 * 1024,
        .disk_status_check_enable = true
    };
    
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mountPoint.c_str(), &host, 
                                            &slot_config, &mount_config, &card);
    
    if (ret == ESP_OK) {
        mounted = true;
        printInfo();
    } else {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(ret));
    }
    
    return ret;
}

esp_err_t SdCardManager::unmount() {
    if (!mounted) return ESP_OK;
    
    esp_err_t ret = esp_vfs_fat_sdcard_unmount(mountPoint.c_str(), card);
    if (ret == ESP_OK) {
        mounted = false;
        card = nullptr;
        ESP_LOGI(TAG, "Unmounted");
    }
    return ret;
}


void SdCardManager::printInfo() const {
    if (!mounted || !card) return;
    
    ESP_LOGI(TAG, "=== SD Card Info ===");
    ESP_LOGI(TAG, "Name: %s", card->cid.name);
    ESP_LOGI(TAG, "Speed: %d kHz", card->max_freq_khz);
    ESP_LOGI(TAG, "Size: %llu MB", 
             ((uint64_t)card->csd.capacity) * card->csd.sector_size / (1024 * 1024));
    
    uint64_t total, free;
    if (getInfo(total, free) == ESP_OK) {
        ESP_LOGI(TAG, "Free: %llu MB / %llu MB", 
                 free / (1024 * 1024), total / (1024 * 1024));
    }
}

// ==================== Video Write Timer ====================

VideoWriteTimer::VideoWriteTimer(VideoManager& mgr)
    : videoMgr(mgr), timerHandle(nullptr), isRunning(false), 
      currentDurationMs(0), currentFps(0) {
    
    mutex = xSemaphoreCreateMutex();
}

VideoWriteTimer::~VideoWriteTimer() {
    stop();
    if (mutex) {
        vSemaphoreDelete(mutex);
    }
}

void VideoWriteTimer::timerCallback(TimerHandle_t xTimer) {
    VideoWriteTimer* self = static_cast<VideoWriteTimer*>(pvTimerGetTimerID(xTimer));
    if (self) {
        self->onTimeout();
    }
}

void VideoWriteTimer::onTimeout() {
    ESP_LOGI(TAG, "Timer timeout - stopping video write");
    stop();
}

esp_err_t VideoWriteTimer::start(const RtcTime& ts, uint32_t durationMs, uint8_t fps) {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (isRunning) {
        // Đang chạy → reset timer
        ESP_LOGI(TAG, "Write already running, resetting timer");
        xSemaphoreGive(mutex);
        return reset();
    }
    
    // Bắt đầu mới
    currentTimestamp = ts;
    currentDurationMs = durationMs;
    currentFps = fps;
    
    // Tạo timer 10 giây
    if (timerHandle == nullptr) {
        timerHandle = xTimerCreate(
            "write_timer",
            pdMS_TO_TICKS(WRITE_TIMEOUT_MS),
            pdFALSE,  // One-shot
            this,
            timerCallback
        );
        
        if (timerHandle == nullptr) {
            ESP_LOGE(TAG, "Failed to create timer");
            xSemaphoreGive(mutex);
            return ESP_FAIL;
        }
    }
    
    // Start timer
    if (xTimerStart(timerHandle, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to start timer");
        xSemaphoreGive(mutex);
        return ESP_FAIL;
    }
    
    isRunning = true;
    xSemaphoreGive(mutex);
    
    ESP_LOGI(TAG, "Started write timer (10s timeout)");
    
    // Bắt đầu ghi video (async)
    return startWriteTask();
}

esp_err_t VideoWriteTimer::reset() {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (!isRunning || timerHandle == nullptr) {
        xSemaphoreGive(mutex);
        return ESP_ERR_INVALID_STATE;
    }
    
    // Reset timer về 10s
    if (xTimerReset(timerHandle, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to reset timer");
        xSemaphoreGive(mutex);
        return ESP_FAIL;
    }
    
    xSemaphoreGive(mutex);
    
    ESP_LOGI(TAG, "Timer reset - extending write time");
    return ESP_OK;
}

esp_err_t VideoWriteTimer::stop() {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_FAIL;
    }
    
    if (!isRunning) {
        xSemaphoreGive(mutex);
        return ESP_OK;
    }
    
    // Stop timer
    if (timerHandle != nullptr) {
        xTimerStop(timerHandle, 0);
        xTimerDelete(timerHandle, 0);
        timerHandle = nullptr;
    }
    
    isRunning = false;
    
    xSemaphoreGive(mutex);
    
    ESP_LOGI(TAG, "Write timer stopped");
    
    // Stop write task nếu đang chạy
    stopWriteTask();
    
    return ESP_OK;
}

esp_err_t VideoWriteTimer::startWriteTask() {
    // Tạo task để ghi video
    BaseType_t ret = xTaskCreate(
        writeTaskFunc,
        "video_write",
        8192,
        this,
        PRIORITY_WRITE_TASK,
        &writeTaskHandle
    );
    
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create write task");
        return ESP_FAIL;
    }
    
    return ESP_OK;
}

void VideoWriteTimer::stopWriteTask() {
    if (writeTaskHandle != nullptr) {
        vTaskDelete(writeTaskHandle);
        writeTaskHandle = nullptr;
    }
}

void VideoWriteTimer::writeTaskFunc(void* param) {
    VideoWriteTimer* self = static_cast<VideoWriteTimer*>(param);
    
    ESP_LOGI(TAG, "Write task started");
    
    VideoInfo info;
    esp_err_t ret = self->videoMgr.writeVideo(
        self->currentTimestamp,
        self->currentDurationMs,
        self->currentFps,
        info
    );
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Video written: %s (%u frames, %u bytes)",
                 info.folderName.c_str(), info.frameCount, info.totalSize);
    } else {
        ESP_LOGE(TAG, "Video write failed");
    }
    
    // Task tự cleanup
    self->writeTaskHandle = nullptr;
    vTaskDelete(nullptr);
}

bool VideoWriteTimer::isActive() const {
    bool active = false;
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        active = isRunning;
        xSemaphoreGive(mutex);
    }
    return active;
}

// ==================== Video Manager ====================

VideoManager::VideoManager(SdCardManager& sd, const std::string& root)
    : sdCard(sd), rootPath(root) {}

esp_err_t VideoManager::init() {
    if (!sdCard.isMounted()) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Create root directory
    struct stat st;
    if (stat(rootPath.c_str(), &st) == -1) {
        if (mkdir(rootPath.c_str(), 0755) == -1) {
            ESP_LOGE(TAG, "Failed to create root dir: %s", rootPath.c_str());
            return ESP_FAIL;
        }
        ESP_LOGI(TAG, "Created root dir: %s", rootPath.c_str());
    }
    
    return ESP_OK;
}

esp_err_t VideoManager::createFolder(const std::string& path) {
    if (mkdir(path.c_str(), 0755) == -1) {
        ESP_LOGE(TAG, "Failed to create folder: %s", path.c_str());
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t VideoManager::writeVideo(const RtcTime& timestamp, 
                                   uint32_t durationMs, 
                                   uint8_t fps,
                                   VideoInfo& videoInfo) {
    
    if (!sdCard.isMounted()) {
        ESP_LOGE(TAG, "SD card not mounted");
        return ESP_ERR_INVALID_STATE;
    }
    
    // Check free space
    uint64_t free;

    
    // Create folder
    std::string folderName = RtcTimeUtils::toFolderName(timestamp);
    std::string folderPath = rootPath + "/" + folderName;
    
    if (createFolder(folderPath) != ESP_OK) {
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Recording to: %s", folderPath.c_str());
    
    uint32_t totalFrames = (durationMs * fps) / 1000;
    uint32_t delayMs = 1000 / fps;
    uint32_t frameCount = 0;
    uint32_t totalSize = 0;
    
    // Capture frames
    for (uint32_t i = 0; i < totalFrames; i++) {
        camera_fb_t* fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGE(TAG, "Capture failed at frame %u", i);
            continue;
        }
        
        char filename[256];
        snprintf(filename, sizeof(filename), "%s/%04u.jpg", 
                 folderPath.c_str(), i + 1);
        
        FILE* file = fopen(filename, "wb");
        if (file) {
            size_t written = fwrite(fb->buf, 1, fb->len, file);
            fflush(file);
            fclose(file);
            
            if (written == fb->len) {
                frameCount++;
                totalSize += fb->len;
            }
        }
        
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(delayMs));
    }
    
    sync();
    
    videoInfo.folderName = folderName;
    videoInfo.fullPath = folderPath;
    videoInfo.frameCount = frameCount;
    videoInfo.totalSize = totalSize;
    
    ESP_LOGI(TAG, "Recording completed: %u frames, %u bytes", frameCount, totalSize);
    
    return ESP_OK;
}

esp_err_t VideoManager::readVideo(const std::string& folderPath) {
    ESP_LOGI(TAG, "Reading video from: %s", folderPath.c_str());
    
    DIR* dir = opendir(folderPath.c_str());
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open dir: %s", folderPath.c_str());
        return ESP_FAIL;
    }
    
    uint32_t fileCount = 0;
    uint32_t successCount = 0;
    struct dirent* entry;
    
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, ".jpg") == nullptr) {
            continue;
        }
        
        fileCount++;
        
        std::string filepath = folderPath + "/" + entry->d_name;
        
        if (uploadFile(filepath) == ESP_OK) {
            successCount++;
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    
    closedir(dir);
    
    ESP_LOGI(TAG, "Upload completed: %u/%u files", successCount, fileCount);
    
    return (successCount == fileCount) ? ESP_OK : ESP_FAIL;
}

esp_err_t VideoManager::uploadFile(const std::string& filepath) {
    FILE* file = fopen(filepath.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "Failed to open: %s", filepath.c_str());
        return ESP_FAIL;
    }
    
    fseek(file, 0, SEEK_END);
    long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    uint8_t* buffer = (uint8_t*)malloc(fileSize);
    if (!buffer) {
        fclose(file);
        return ESP_FAIL;
    }
    
    fread(buffer, 1, fileSize, file);
    fclose(file);
    
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%d%s", SERVER_IP, SERVER_PORT, UPLOAD_ENDPOINT);
    
    esp_http_client_config_t config = {};
    config.url = url;
    config.method = HTTP_METHOD_POST;
    config.timeout_ms = 10000;
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "image/jpeg");
    esp_http_client_set_post_field(client, (const char*)buffer, fileSize);
    
    esp_err_t ret = esp_http_client_perform(client);
    
    if (ret == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "Uploaded: %s - Status: %d", filepath.c_str(), status);
    }
    
    esp_http_client_cleanup(client);
    free(buffer);
    
    return ret;
}

esp_err_t VideoManager::deleteOldVideos(const RtcTime& currentTime, uint32_t daysOld) {
    RtcTime threshold = RtcTimeUtils::daysAgo(currentTime, daysOld);
    
    DIR* dir = opendir(rootPath.c_str());
    if (!dir) {
        return ESP_FAIL;
    }
    
    struct dirent* entry;
    uint32_t deletedCount = 0;
    
    while ((entry = readdir(dir)) != nullptr) {
        if (strlen(entry->d_name) != 14) continue;
        
        std::string folderPath = rootPath + "/" + entry->d_name;
        
        // Compare with threshold
        if (std::string(entry->d_name) < RtcTimeUtils::toFolderName(threshold)) {
            if (deleteFolder(folderPath) == ESP_OK) {
                deletedCount++;
            }
        }
    }
    
    closedir(dir);
    
    ESP_LOGI(TAG, "Deleted %u old folders", deletedCount);
    return ESP_OK;
}

esp_err_t VideoManager::deleteFolder(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return ESP_FAIL;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        std::string filepath = path + "/" + entry->d_name;
        remove(filepath.c_str());
    }
    
    closedir(dir);
    rmdir(path.c_str());
    
    return ESP_OK;
}

std::vector<VideoInfo> VideoManager::listVideos() {
    std::vector<VideoInfo> videos;
    
    DIR* dir = opendir(rootPath.c_str());
    if (!dir) return videos;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strlen(entry->d_name) == 14) {
            VideoInfo info;
            info.folderName = entry->d_name;
            info.fullPath = rootPath + "/" + entry->d_name;
            videos.push_back(info);
        }
    }
    
    closedir(dir);
    return videos;
}