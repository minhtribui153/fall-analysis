idf_component_register(
    SRCS
        "main.cpp"
        "ble_advertiser.cpp"
        "gatt_server.cpp"
    INCLUDE_DIRS
        "."

    REQUIRES
        bt
        nvs_flash
        esp_hw_support
)