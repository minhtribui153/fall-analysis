set(TERVANIX_SRCS
    "main.cpp"
    "ble_advertiser.cpp"
    "identity_store.cpp"
    "device_identity.cpp"
    "wifi_manager.cpp"
    "mqtt_publisher.cpp"
    "health_display.cpp"
    "nesso_board_io.cpp"
    "imu/imu_sensor.cpp"
)

set(TERVANIX_REQUIRES
    bt
    nvs_flash
    esp_hw_support
    esp_wifi
    esp_netif
    esp_event
    esp_timer
    mqtt
    json
    mbedtls
    driver
    esp_driver_i2c
    esp_lcd
)

idf_component_register(
    SRCS ${TERVANIX_SRCS}
    INCLUDE_DIRS "."
    REQUIRES ${TERVANIX_REQUIRES}
)
