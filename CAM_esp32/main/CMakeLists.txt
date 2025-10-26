idf_component_register(SRCS "CAM_WiFi.cpp" "CAM_NVS.cpp" "main.cpp" "CAM_mqttApi.cpp" "CAM_config.cpp" "CAM_HTTPstream.cpp" "CAM_memorFunc.cpp" "CAM_sensorRead.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES
                        "esp_psram"
                    REQUIRES 
                        "driver"
                        "esp32-camera"
                        "fatfs"
                        "vfs"
                        "sdmmc"
                        "freertos"
                        "esp_timer"
                        "esp_http_client"
                        "esp_http_server"
                        "mqtt"
                        "nvs_flash"
                        "esp_wifi"
                        #"bt"
                        )
                        
target_compile_options(${COMPONENT_LIB} PRIVATE 
    -ffunction-sections 
    -fdata-sections
    -fno-tree-switch-conversion
    -fno-jump-tables
)

target_link_libraries(${COMPONENT_LIB} PRIVATE
    "-Wl,--gc-sections"
)
idf_build_set_property(PARTITION_TABLE_FILE "partitions.csv" APPEND)
