// ---- CAM_WiFi_NVS.hpp ----

#pragma once
#include <string>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "NimBLEDevice.h"
class MqttApiManager;

class CAM_WiFi_NVS {
public:
    explicit CAM_WiFi_NVS(MqttApiManager* mqtt);
    void init();
    void connectWiFi();
    void saveWiFiConfig(const std::string& s, const std::string& p, const std::string& t);

private:
    void loadWiFiConfig();
    void startBLE();
    static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

    MqttApiManager* mqttManager;
    char ssid[64];
    char password[64];
    char token[128];
};

class WiFiReceiveCallback : public NimBLECharacteristicCallbacks {
public:
    explicit WiFiReceiveCallback(CAM_WiFi_NVS* parent_) : parent(parent_) {}
    void onWrite(NimBLECharacteristic* pCharacteristic) override;
private:
    CAM_WiFi_NVS* parent;
};

// ---- MqttApiManager update ----
void MqttApiManager::publishWiFiStatus(const std::string& status) {
    if (mqttClient && mqttConnected) {
        mqttClient->publish(topicWiFiPub.c_str(), status.c_str(), status.length(), 1, false);
        ESP_LOGI(TAG, "Published WiFi status: %s", status.c_str());
    }
}