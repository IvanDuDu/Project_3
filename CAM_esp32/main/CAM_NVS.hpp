#ifndef CAM_NVS_HPP
#define CAM_NVS_HPP

#include "esp_err.h"
#include "nvs_flash.h"
#include <string>

// WiFi Credentials structure
struct WiFiCredentials {
    std::string ssid;
    std::string password;
    std::string token;
    
    WiFiCredentials() = default;
    WiFiCredentials(const std::string& s, const std::string& p, const std::string& t)
        : ssid(s), password(p), token(t) {}
    
    bool isEmpty() const {
        return ssid.empty() || password.empty();
    }
};

// NVS Manager for WiFi credentials
class NvsManager {
private:
    static const char* TAG;
    static constexpr const char* NVS_NAMESPACE = "wifi_config";
    static constexpr const char* KEY_SSID = "ssid";
    static constexpr const char* KEY_PASSWORD = "password";
    static constexpr const char* KEY_TOKEN = "token";
    
public:
    NvsManager();
    ~NvsManager() = default;
    
    esp_err_t init();
    
    esp_err_t saveCredentials(const WiFiCredentials& creds);
    esp_err_t loadCredentials(WiFiCredentials& creds);
    esp_err_t clearCredentials();
    
    bool hasCredentials();
};

#endif // CAM_NVS_HPP