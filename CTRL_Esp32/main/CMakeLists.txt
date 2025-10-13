idf_component_register(SRCS "CTRL_WifiConnect.cpp" "CTRL_blueToothConnect.c" "CTRL_config.c" "CTRL_CamUart.c" "CTRL_MQTTServo.c" "main.c"
                    INCLUDE_DIRS ""
                    REQUIRES
                    "esp_wifi"
                    "esp_event"
                    )