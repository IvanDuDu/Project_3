#include "CAM_WiFi.hpp"
#include "CAM_mqttApi.hpp"
#include "esp_log.h"
#include "esp_netif.h"
#include <cstring>


// ==================== BLE Provisioning Manager ====================

#include "CAM_WiFi.hpp"
#include "CAM_mqttApi.hpp"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include <cstring>
#include <sys/param.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

const char* NimBleProvisioningManager::TAG = "WIFI_PROV";
NimBleProvisioningManager::NimBleProvisioningManager()
    : apStarted(false), provisioningComplete(false), serverTaskHandle(nullptr),
      lastClientSocket(-1) {
}

NimBleProvisioningManager::~NimBleProvisioningManager() {
    stop();
}

esp_err_t NimBleProvisioningManager::start() {
    ESP_LOGI(TAG, "Starting WiFi AP provisioning...");
    startSoftAP();

    // create a FreeRTOS task to run TCP server
    if (xTaskCreate(provisioningServerTask, "prov_server", 4096, this, 5, &serverTaskHandle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create provisioning server task");
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t NimBleProvisioningManager::stop() {
    ESP_LOGI(TAG, "Stopping provisioning manager...");
    // stop server task
    if (serverTaskHandle) {
        // signal the task to end by shutting down last client socket and letting accept fail
        if (lastClientSocket >= 0) {
            shutdown(lastClientSocket, SHUT_RDWR);
            close(lastClientSocket);
            lastClientSocket = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        vTaskDelete(serverTaskHandle);
        serverTaskHandle = nullptr;
    }

    stopSoftAP();
    provisioningComplete = false;
    return ESP_OK;
}

void NimBleProvisioningManager::startSoftAP() {
    if (apStarted) return;

    ESP_LOGI(TAG, "Configuring SoftAP: %s", DEVICE_NAME);

    // ensure netif for AP exists
    esp_netif_create_default_wifi_ap();

    wifi_config_t ap_config = {};
    strncpy((char*)ap_config.ap.ssid, DEVICE_NAME, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(DEVICE_NAME);
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN; // open for provisioning: consider PSK if you want
    ap_config.ap.channel = 1;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP started (SSID='%s')", DEVICE_NAME);
    apStarted = true;
}

void NimBleProvisioningManager::stopSoftAP() {
    if (!apStarted) return;

    ESP_ERROR_CHECK(esp_wifi_stop());
    apStarted = false;
    ESP_LOGI(TAG, "SoftAP stopped");
}

// Simple parser: expected "ssid//password//token"
esp_err_t NimBleProvisioningManager::parseCredentials(const std::string& data) {
    size_t pos1 = data.find("//");
    if (pos1 == std::string::npos) {
        ESP_LOGE(TAG, "Invalid format: missing first //");
        return ESP_FAIL;
    }

    size_t pos2 = data.find("//", pos1 + 2);
    if (pos2 == std::string::npos) {
        ESP_LOGE(TAG, "Invalid format: missing second //");
        return ESP_FAIL;
    }

    receivedCredentials.ssid = data.substr(0, pos1);
    receivedCredentials.password = data.substr(pos1 + 2, pos2 - pos1 - 2);
    receivedCredentials.token = data.substr(pos2 + 2);

    ESP_LOGI(TAG, "Parsed - SSID: %s, Token: %s",
             receivedCredentials.ssid.c_str(), receivedCredentials.token.c_str());

    return ESP_OK;
}

esp_err_t NimBleProvisioningManager::sendAck() {
    // If we have a connected client socket, send ACK
    if (lastClientSocket < 0) {
        ESP_LOGW(TAG, "No client socket to send ACK");
        return ESP_ERR_INVALID_STATE;
    }

    const char* ack_msg = "ACK";
    ssize_t sent = send(lastClientSocket, ack_msg, strlen(ack_msg), 0);
    if (sent < 0) {
        ESP_LOGE(TAG, "Failed to send ACK");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "ACK sent to mobile app over TCP");
    return ESP_OK;
}

static ssize_t recv_all(int sockfd, char* buf, size_t buflen, uint32_t timeout_ms) {
    // set recv timeout
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ssize_t total = 0;
    ssize_t r;
    // one-shot read (we expect small payload), read up to buflen
    r = recv(sockfd, buf, buflen - 1, 0);
    if (r > 0) total = r;
    return total;
}

void NimBleProvisioningManager::provisioningServerTask(void* param) {
    NimBleProvisioningManager* self = static_cast<NimBleProvisioningManager*>(param);
    const int listen_port = NimBleProvisioningManager::PROV_PORT;

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(NimBleProvisioningManager::TAG, "Failed to create listen socket");
        vTaskDelete(nullptr);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(listen_port);

    if (bind(listen_sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(NimBleProvisioningManager::TAG, "Failed to bind listen socket");
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    if (listen(listen_sock, 1) < 0) {
        ESP_LOGE(NimBleProvisioningManager::TAG, "Listen failed");
        close(listen_sock);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(NimBleProvisioningManager::TAG, "Provisioning TCP server listening on port %d", listen_port);

    while (true) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr*)&client_addr, &addr_len);
        if (client_sock < 0) {
            ESP_LOGW(NimBleProvisioningManager::TAG, "Accept failed or socket closed");
            break;
        }

        char ipstr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ipstr, sizeof(ipstr));
        ESP_LOGI(NimBleProvisioningManager::TAG, "Client connected: %s:%d", ipstr, ntohs(client_addr.sin_port));

        // store for sendAck()
        self->lastClientSocket = client_sock;

        // receive data (small payload expected)
        char buf[512];
        memset(buf, 0, sizeof(buf));
        ssize_t r = recv_all(client_sock, buf, sizeof(buf), 10000); // 10s timeout
        if (r <= 0) {
            ESP_LOGW(NimBleProvisioningManager::TAG, "No data received or timeout");
            close(client_sock);
            self->lastClientSocket = -1;
            continue;
        }

        std::string received(buf, (size_t)r);
        ESP_LOGI(NimBleProvisioningManager::TAG, "Received provisioning data: %s", received.c_str());

        if (self->parseCredentials(received) == ESP_OK) {
            // mark complete so caller will proceed to connect as STA
            self->provisioningComplete = true;

            // Reply ACK to client
            const char* ack = "ACK";
            send(client_sock, ack, strlen(ack), 0);
            ESP_LOGI(NimBleProvisioningManager::TAG, "Sent ACK to mobile");

            // keep connection briefly to ensure client reads
            vTaskDelay(pdMS_TO_TICKS(500));

            // close client socket
            shutdown(client_sock, SHUT_RDWR);
            close(client_sock);
            self->lastClientSocket = -1;

            // stop listening (we provisioned successfully)
            break;
        } else {
            const char* nak = "NACK";
            send(client_sock, nak, strlen(nak), 0);
            shutdown(client_sock, SHUT_RDWR);
            close(client_sock);
            self->lastClientSocket = -1;
            continue;
        }
    }

    // close listen socket
    close(listen_sock);
    ESP_LOGI(NimBleProvisioningManager::TAG, "Provisioning server task exiting");

    // optionally stop AP here or leave for caller to stop after processing
    // self->stopSoftAP();

    vTaskDelete(nullptr);
}


// ==================== WiFi Connection Manager ====================

const char* WiFiConnectionManager::TAG = "WIFI_MGR";
WiFiConnectionManager* WiFiConnectionManager::instance = nullptr;

WiFiConnectionManager::WiFiConnectionManager()
    : bleManager(nullptr), mqttApi(nullptr), currentState(WiFiState::DISCONNECTED),
      monitorTaskHandle(nullptr) {
    
    wifiEventGroup = xEventGroupCreate();
    instance = this;
}

WiFiConnectionManager::~WiFiConnectionManager() {
    disconnect();
    
    if (bleManager != nullptr) {
        delete bleManager;
    }
    
    if (wifiEventGroup != nullptr) {
        vEventGroupDelete(wifiEventGroup);
    }
    
    instance = nullptr;
}

esp_err_t WiFiConnectionManager::init() {
    ESP_LOGI(TAG, "Initializing WiFi Manager");
    
    esp_err_t ret = nvsManager.init();
    if (ret != ESP_OK) {
        return ret;
    }
    
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, 
                                               &wifiEventHandler, this));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifiEventHandler, this));
    
    ESP_LOGI(TAG, "WiFi initialized");
    return ESP_OK;
}

esp_err_t WiFiConnectionManager::start() {
    ESP_LOGI(TAG, "Starting WiFi connection process");
    
    if (nvsManager.loadCredentials(credentials) == ESP_OK && !credentials.isEmpty()) {
        ESP_LOGI(TAG, "Found credentials in NVS");
        
        if (tokenCallback) {
            tokenCallback(credentials.token);
        }
        
        return connectToWiFi();
    } else {
        ESP_LOGI(TAG, "No credentials found, starting BLE provisioning");
        return startBleProvisioning();
    }
}

esp_err_t WiFiConnectionManager::connectToWiFi() {
    currentState = WiFiState::CONNECTING;
    
    if (stateCallback) {
        stateCallback(currentState);
    }
    
    wifi_config_t wifi_config = {};
    strncpy((char*)wifi_config.sta.ssid, credentials.ssid.c_str(), sizeof(wifi_config.sta.ssid));
    strncpy((char*)wifi_config.sta.password, credentials.password.c_str(), sizeof(wifi_config.sta.password));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
    
    ESP_LOGI(TAG, "Connecting to WiFi: %s", credentials.ssid.c_str());
    
    EventBits_t bits = xEventGroupWaitBits(wifiEventGroup,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(15000));
    
    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected successfully");
        currentState = WiFiState::CONNECTED;
        
        if (stateCallback) {
            stateCallback(currentState);
        }
        
        mqttApi->publishStatus("active");
        
        xTaskCreate(monitorTaskFunc, "wifi_monitor", 4096, this, 4, &monitorTaskHandle);
        
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "Failed to connect to WiFi");
        currentState = WiFiState::FAILED;
        
        if (stateCallback) {
            stateCallback(currentState);
        }
        
        return ESP_FAIL;
    }
}

esp_err_t WiFiConnectionManager::startBleProvisioning() {
    bleManager = new NimBleProvisioningManager();
    esp_err_t ret = bleManager->start();
    
    if (ret != ESP_OK) {
        return ret;
    }
    
    ESP_LOGI(TAG, "Waiting for BLE provisioning...");
    
    while (!bleManager->isProvisioningComplete()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    
    credentials = bleManager->getCredentials();
    nvsManager.saveCredentials(credentials);
    
    if (tokenCallback) {
        tokenCallback(credentials.token);
    }
    
    ret = connectToWiFi();
    
    if (ret == ESP_OK) {
        bleManager->sendAck();
        vTaskDelay(pdMS_TO_TICKS(2000));
        bleManager->stop();
        delete bleManager;
        bleManager = nullptr;
    }
    
    return ret;
}

void WiFiConnectionManager::wifiEventHandler(void* arg, esp_event_base_t event_base,
                                            int32_t event_id, void* event_data) {
    WiFiConnectionManager* self = static_cast<WiFiConnectionManager*>(arg);
    
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG, "WiFi started");
        
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi disconnected");
        
        self->currentState = WiFiState::DISCONNECTED;
        
        if (self->stateCallback) {
            self->stateCallback(self->currentState);
        }
        
        xEventGroupSetBits(self->wifiEventGroup, WIFI_FAIL_BIT);
        
        
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        
        xEventGroupSetBits(self->wifiEventGroup, WIFI_CONNECTED_BIT);

        static bool firstConnection = true;
    if (firstConnection) {
        self->mqttApi->publishStatus("active", self->mqttApi->getTopicWiFiPub());
        firstConnection = false;
    } else {
        self->mqttApi->publishStatus("connected", self->mqttApi->getTopicWiFiPub());
    }
        
    }
}

void WiFiConnectionManager::monitorTaskFunc(void* param) {
    WiFiConnectionManager* self = static_cast<WiFiConnectionManager*>(param);
    
    ESP_LOGI(TAG, "WiFi monitor task started");
    
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        
        if (self->currentState == WiFiState::DISCONNECTED) {
            ESP_LOGI(TAG, "Attempting reconnect...");
            esp_wifi_connect();
            
        } else if (self->currentState == WiFiState::CONNECTED) {
            wifi_ap_record_t ap_info;
            if (esp_wifi_sta_get_ap_info(&ap_info) != ESP_OK) {
                ESP_LOGW(TAG, "Lost WiFi connection");
                self->currentState = WiFiState::DISCONNECTED;
                self->mqttApi->publishStatus("inactive");
            }
        }
    }
}


esp_err_t WiFiConnectionManager::disconnect() {
    ESP_LOGI(TAG, "Disconnecting WiFi");
    
    currentState = WiFiState::DISCONNECTED;
    
    if (monitorTaskHandle != nullptr) {
        vTaskDelete(monitorTaskHandle);
        monitorTaskHandle = nullptr;
    }
    
    esp_wifi_disconnect();
    esp_wifi_stop();
    
    return ESP_OK;
}

esp_err_t WiFiConnectionManager::setCredentials(const WiFiCredentials& creds) {
    credentials = creds;
    nvsManager.saveCredentials(creds);
    
    if (tokenCallback) {
        tokenCallback(credentials.token);
    }
    
    return ESP_OK;
}

esp_err_t WiFiConnectionManager::resetCredentials() {
    ESP_LOGI(TAG, "Resetting credentials");
    
    disconnect();
    nvsManager.clearCredentials();
    credentials = WiFiCredentials();
    
    ESP_LOGI(TAG, "Credentials reset. Restart for BLE provisioning");
    return ESP_OK;
}