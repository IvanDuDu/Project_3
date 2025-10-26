#include "CAM_NVS.hpp"
#include "esp_log.h"
#include "nvs.h"
#include <cstring>

const char* NvsManager::TAG = "NVS_MGR";

NvsManager::NvsManager() {}

esp_err_t NvsManager::init() {
    esp_err_t ret = nvs_flash_init();
    
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "NVS initialized");
    }
    
    return ret;
}

esp_err_t NvsManager::saveCredentials(const WiFiCredentials& creds) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS");
        return ret;
    }
    
    ret = nvs_set_str(handle, KEY_SSID, creds.ssid.c_str());
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_str(handle, KEY_PASSWORD, creds.password.c_str());
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_set_str(handle, KEY_TOKEN, creds.token.c_str());
    if (ret != ESP_OK) goto cleanup;
    
    ret = nvs_commit(handle);
    
cleanup:
    nvs_close(handle);
    
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Credentials saved: SSID=%s, Token=%s", 
                 creds.ssid.c_str(), creds.token.c_str());
    }
    
    return ret;
}

esp_err_t NvsManager::loadCredentials(WiFiCredentials& creds) {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    char buffer[128];
    size_t len;
    
    len = sizeof(buffer);
    ret = nvs_get_str(handle, KEY_SSID, buffer, &len);
    if (ret == ESP_OK) {
        creds.ssid = std::string(buffer);
    } else {
        nvs_close(handle);
        return ret;
    }
    
    len = sizeof(buffer);
    ret = nvs_get_str(handle, KEY_PASSWORD, buffer, &len);
    if (ret == ESP_OK) {
        creds.password = std::string(buffer);
    } else {
        nvs_close(handle);
        return ret;
    }
    
    len = sizeof(buffer);
    ret = nvs_get_str(handle, KEY_TOKEN, buffer, &len);
    if (ret == ESP_OK) {
        creds.token = std::string(buffer);
    } else {
        nvs_close(handle);
        return ret;
    }
    
    nvs_close(handle);
    
    ESP_LOGI(TAG, "Credentials loaded from NVS");
    return ESP_OK;
}

esp_err_t NvsManager::clearCredentials() {
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    nvs_erase_all(handle);
    ret = nvs_commit(handle);
    nvs_close(handle);
    
    ESP_LOGI(TAG, "Credentials cleared");
    return ret;
}

bool NvsManager::hasCredentials() {
    WiFiCredentials creds;
    return loadCredentials(creds) == ESP_OK && !creds.isEmpty();
}