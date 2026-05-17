idf_component_register(
    SRCS         "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES     nvs_flash
                 spiffs
                 esp_timer
                 esp-sr
)
