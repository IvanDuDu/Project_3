#ifndef CAM_WIFI_HPP
#define CAM_WIFI_HPP

#include "CAM_NVS.hpp"
#include "esp_err.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include <string>
#include <functional>
#include <sys/socket.h>
#include <netinet/in.h>

// Forward declaration
class MqttApiManager;

// WiFi Connection State
enum class WiFiState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    FAILED
};

// NOTE: class name kept as NimBleProvisioningManager to minimize changes elsewhere.
// Implementation will use WiFi AP + TCP provisioning instead of BLE.
class NimBleProvisioningManager {
private:
    bool apStarted;
    bool provisioningComplete;
    WiFiCredentials receivedCredentials;

    TaskHandle_t serverTaskHandle;
    static const char* TAG;
    static constexpr const char* DEVICE_NAME = "ESP32_CAM_Setup";
    static constexpr int PROV_PORT = 8000;

    // internal helpers
    void startSoftAP();
    void stopSoftAP();
    static void provisioningServerTask(void* param);
    esp_err_t parseCredentials(const std::string& data);

public:
    NimBleProvisioningManager();
    ~NimBleProvisioningManager();

    esp_err_t start(); // start AP + server task
    esp_err_t stop();  // stop server + AP

    bool isProvisioningComplete() const { return provisioningComplete; }
    WiFiCredentials getCredentials() const { return receivedCredentials; }

    // send ack back to mobile (over TCP — we will store last client socket fd)
    esp_err_t sendAck();

    // (optional) track last client socket for replies
    int lastClientSocket;
};



// WiFi Connection Manager with MQTT integration
class WiFiConnectionManager {
private:
    NvsManager nvsManager;
    NimBleProvisioningManager* bleManager; // now actually WiFi AP provisioning manager
    MqttApiManager* mqttApi;

    WiFiState currentState;
    WiFiCredentials credentials;

    EventGroupHandle_t wifiEventGroup;
    TaskHandle_t monitorTaskHandle;

    static const char* TAG;
    static constexpr int WIFI_CONNECTED_BIT = (1 << 0);
    static constexpr int WIFI_FAIL_BIT = (1 << 1);

    std::function<void(WiFiState)> stateCallback;
    std::function<void(const std::string&)> tokenCallback;

    static WiFiConnectionManager* instance;

    static void wifiEventHandler(void* arg, esp_event_base_t event_base,
                                 int32_t event_id, void* event_data);

    static void monitorTaskFunc(void* param);

    esp_err_t connectToWiFi();
    esp_err_t startBleProvisioning(); // will call NimBleProvisioningManager

public:
    WiFiConnectionManager();
    ~WiFiConnectionManager();

    WiFiConnectionManager(const WiFiConnectionManager&) = delete;
    WiFiConnectionManager& operator=(const WiFiConnectionManager&) = delete;

    esp_err_t init();
    esp_err_t start();
    esp_err_t disconnect();

    void setMqttApi(MqttApiManager* mqtt) { mqttApi = mqtt; }

    WiFiState getState() const { return currentState; }
    bool isConnected() const { return currentState == WiFiState::CONNECTED; }
    std::string getToken() const { return credentials.token; }
    std::string getSsid() const { return credentials.ssid; }

    void onStateChange(std::function<void(WiFiState)> callback) {
        stateCallback = callback;
    }

    void onTokenReceived(std::function<void(const std::string&)> callback) {
        tokenCallback = callback;
    }

    esp_err_t setCredentials(const WiFiCredentials& creds);
    esp_err_t resetCredentials();
};

#endif // CAM_WIFI_HPP
