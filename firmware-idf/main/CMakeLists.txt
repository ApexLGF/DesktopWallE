idf_component_register(
    SRCS         "main.cpp"
                 "mic.cpp"
                 "pmu.cpp"
                 "wifi_sta.cpp"
                 "bridge_ws.cpp"
    INCLUDE_DIRS "."
    REQUIRES     nvs_flash
                 spiffs
                 esp_timer
                 esp_event
                 esp_netif
                 esp_wifi
                 esp_driver_gpio
                 esp_driver_i2c
                 esp_driver_i2s
                 esp-sr
)
