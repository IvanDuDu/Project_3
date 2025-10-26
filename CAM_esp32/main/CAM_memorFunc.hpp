#ifndef CAM_MEMOR_FUNC_HPP
#define CAM_MEMOR_FUNC_HPP

#include "CAM_sensorRead.hpp"  // Dùng RtcTime từ đây
#include "esp_err.h"
#include "esp_camera.h"
#include "sdmmc_cmd.h"
#include <string>
#include <vector>
#include <cstdint>
#include <functional>

// Extension methods cho RtcTime
namespace RtcTimeUtils {
    std::string toFolderName(const RtcTime& time);
    RtcTime daysAgo(const RtcTime& time, uint32_t days);
    bool isOlderThan(const RtcTime& t1, const RtcTime& t2);
    bool isSameDay(const RtcTime& t1, const RtcTime& t2);
}

// Video information
struct VideoInfo {
    std::string folderName;
    std::string fullPath;
    uint32_t frameCount;
    uint32_t totalSize;
    
    VideoInfo() : frameCount(0), totalSize(0) {}
};

// SD Card Manager Class
class SdCardManager {
private:
    sdmmc_card_t* card;
    bool mounted;
    std::string mountPoint;
    
    static const char* TAG;
    
public:
    explicit SdCardManager(const std::string& mount_point = "/sdcard");
    ~SdCardManager();
    
    // Disable copy
    SdCardManager(const SdCardManager&) = delete;
    SdCardManager& operator=(const SdCardManager&) = delete;
    
    esp_err_t mount();
    esp_err_t unmount();
    bool isMounted() const { return mounted; }
    
   // esp_err_t getInfo(uint64_t& totalBytes, uint64_t& freeBytes) const;
    void printInfo() const;
};

// Video Manager Class
class VideoManager {
private:
    SdCardManager& sdCard;
    std::string rootPath;
    
    static const char* TAG;
    static constexpr const char* SERVER_IP = "192.168.1.200";
    static constexpr int SERVER_PORT = 80;
    static constexpr const char* UPLOAD_ENDPOINT = "/upload";
    
    esp_err_t createFolder(const std::string& path);
    esp_err_t deleteFolder(const std::string& path);
    esp_err_t uploadFile(const std::string& filepath);
    
public:
    explicit VideoManager(SdCardManager& sd, const std::string& root = "/sdcard/videos");
    ~VideoManager() = default;
    
    esp_err_t init();
    
    // Main functions
    esp_err_t writeVideo(const RtcTime& timestamp, 
                        uint32_t durationMs, 
                        uint8_t fps,
                        VideoInfo& videoInfo);
    
    esp_err_t readVideo(const std::string& folderPath);

    esp_err_t deleteOldVideos(const RtcTime& currentTime, uint32_t daysOld = 3);

    // Utility functions
    std::vector<VideoInfo> listVideos();
  //  bool videoExists(const std::string& folderName) const;
   // std::string getVideoPath(const std::string& folderName) const;
};

// Video Write Timer Class (PIR-triggered with auto-timeout)
class VideoWriteTimer {
private:
    VideoManager& videoMgr;
    TimerHandle_t timerHandle;
    TaskHandle_t writeTaskHandle;
    SemaphoreHandle_t mutex;
    
    bool isRunning;
    RtcTime currentTimestamp;
    uint32_t currentDurationMs;
    uint8_t currentFps;
    
    static const char* TAG;
    static constexpr uint32_t WRITE_TIMEOUT_MS = 10000;  // 10 seconds
    static constexpr uint8_t PRIORITY_WRITE_TASK = 3;     // Low priority
    
    static void timerCallback(TimerHandle_t xTimer);
    static void writeTaskFunc(void* param);
    std::function<void(const std::string&)> onVideoComplete;
    
    void onTimeout();
    esp_err_t startWriteTask();
    void stopWriteTask();
    
    
public:
    explicit VideoWriteTimer(VideoManager& mgr);
    ~VideoWriteTimer();
    
    // Disable copy
    VideoWriteTimer(const VideoWriteTimer&) = delete;
    VideoWriteTimer& operator=(const VideoWriteTimer&) = delete;
    void setOnComplete(std::function<void(const std::string&)> callback) {
        onVideoComplete = callback;
    }
    
    // Main control functions
    esp_err_t start(const RtcTime& timestamp, uint32_t durationMs, uint8_t fps);
    esp_err_t reset();  // Called when PIR triggers again
    esp_err_t stop();
    
    bool isActive() const;

};


// videoWriteTimer.setOnComplete([&mqttApi](const std::string& folderName) {
//     mqttApi.publishFolderName(folderName);
// });

#endif // CAM_MEMOR_FUNC_HPP