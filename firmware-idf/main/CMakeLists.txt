idf_component_register(
    SRCS         "main.cpp"
                 "mic.cpp"
                 "pmu.cpp"
                 "wifi_sta.cpp"
                 "bridge_ws.cpp"
                 "lcd.cpp"
                 "speaker.cpp"
                 "led.cpp"
                 "touch.cpp"
                 "servo.cpp"
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
                 esp_driver_spi
                 esp_driver_uart
                 esp_lcd
                 esp-sr
)
