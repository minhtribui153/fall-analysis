idf_component_register(
    SRCS
        "main.cpp"
        "ble_advertiser.cpp"
        "identity_store.cpp"
        "device_identity.cpp"
        "imu/imu_sensor.cpp"
    INCLUDE_DIRS
        "."

    REQUIRES
        bt
        nvs_flash
        esp_hw_support
        driver
        esp_driver_i2c
)
