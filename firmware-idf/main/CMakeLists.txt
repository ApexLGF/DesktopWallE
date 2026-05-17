idf_component_register(
    SRCS         "main.cpp"
                 "mic.cpp"
    INCLUDE_DIRS "."
    REQUIRES     nvs_flash
                 spiffs
                 esp_timer
                 esp_driver_i2c
                 esp_driver_i2s
                 esp-sr
)
