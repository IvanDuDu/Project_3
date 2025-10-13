#include "CAM_WiFi_NVS.hpp"
#include "CAM_mqttApi.hpp"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_bt.h"
#include "NimBLEDevice.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char* TAG = "CAM_WIFI_NVS";

static EventGroupHandle_t s_wifi_event_group;
static MqttApiManager* mqttApi = nullptr;

CAM_WiFi_NVS::CAM_WiFi_NVS(MqttApiManager* mqtt) : mqttManager(mqtt) {
    mqttApi = mqtt;
}

void CAM_WiFi_NVS::init() {
    nvs_flash_init();
    s_wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    loadWiFiConfig();
}

void CAM_WiFi_NVS::loadWiFiConfig() {
    nvs_handle_t nvsHandle;
    esp_err_t err = nvs_open("wifi_config", NVS_READWRITE, &nvsHandle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        startBLE();
        return;
    }

    size_t ssidLen = sizeof(ssid);
    size_t passLen = sizeof(password);
    size_t tokenLen = sizeof(token);

    err = nvs_get_str(nvsHandle, "ssid", ssid, &ssidLen);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "No SSID in NVS, start BLE");
        nvs_close(nvsHandle);
        startBLE();
        return;
    }

    err = nvs_get_str(nvsHandle, "password", password, &passLen);
    err = nvs_get_str(nvsHandle, "token", token, &tokenLen);

    nvs_close(nvsHandle);
    connectWiFi();
}

void CAM_WiFi_NVS::connectWiFi() {
    ESP_LOGI(TAG, "Connecting to WiFi SSID:%s", ssid);

    wifi_config_t wifi_config = {};
    strcpy((char*)wifi_config.sta.ssid, ssid);
    strcpy((char*)wifi_config.sta.password, password);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, &instance_got_ip);

    ESP_ERROR_CHECK(esp_wifi_connect());

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to WiFi successfully");
        if (mqttManager) {
            mqttManager->publishWiFiStatus("active");
        }
    } else {
        ESP_LOGE(TAG, "Failed to connect WiFi, switching to BLE");
        if (mqttManager) {
            mqttManager->publishWiFiStatus("inactive");
        }
        startBLE();
    }
}

void CAM_WiFi_NVS::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        if (mqttApi) mqttApi->publishWiFiStatus("inactive");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WiFi got IP");
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        if (mqttApi) mqttApi->publishWiFiStatus("active");
    }
}

void CAM_WiFi_NVS::startBLE() {
    ESP_LOGI(TAG, "Starting BLE setup mode");
    NimBLEDevice::init("ESP32_CAM_SETUP");
    NimBLEServer* pServer = NimBLEDevice::createServer();
    NimBLEService* pService = pServer->createService("1234");
    NimBLECharacteristic* pCharacteristic = pService->createCharacteristic("5678", NIMBLE_PROPERTY::WRITE);

    pCharacteristic->setCallbacks(new WiFiReceiveCallback(this));
    pService->start();
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->start();
}

void CAM_WiFi_NVS::saveWiFiConfig(const std::string& s, const std::string& p, const std::string& t) {
    nvs_handle_t nvsHandle;
    nvs_open("wifi_config", NVS_READWRITE, &nvsHandle);
    nvs_set_str(nvsHandle, "ssid", s.c_str());
    nvs_set_str(nvsHandle, "password", p.c_str());
    nvs_set_str(nvsHandle, "token", t.c_str());
    nvs_commit(nvsHandle);
    nvs_close(nvsHandle);
}

void WiFiReceiveCallback::onWrite(NimBLECharacteristic* pCharacteristic) {
    std::string value = pCharacteristic->getValue();
    if (value.empty()) return;

    size_t first = value.find("//");
    size_t second = value.find("//", first + 2);

    std::string s = value.substr(0, first);
    std::string p = value.substr(first + 2, second - (first + 2));
    std::string t = value.substr(second + 2);

    parent->saveWiFiConfig(s, p, t);

    NimBLECharacteristic* ackChar = pCharacteristic->getService()->createCharacteristic("ACK", NIMBLE_PROPERTY::NOTIFY);
    ackChar->setValue("ACK");
    ackChar->notify();

    vTaskDelay(pdMS_TO_TICKS(1000));
    parent->connectWiFi();
}


