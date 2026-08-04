set(TERVANIX_SRCS
    "main.cpp" "ble_advertiser.cpp" "identity_store.cpp" "device_identity.cpp"
    "wifi_manager.cpp" "mqtt_publisher.cpp" "peer_network.cpp"
    "registry_store.cpp" "registration_client.cpp")

set(TERVANIX_REQUIRES bt nvs_flash esp_hw_support esp_wifi esp_netif esp_event esp_timer mqtt json esp_http_client)
if(CONFIG_IDF_TARGET_ESP32C6)
    list(APPEND TERVANIX_SRCS "imu/imu_sensor.cpp")
    list(APPEND TERVANIX_REQUIRES driver esp_driver_i2c bmi270)
endif()

idf_component_register(SRCS ${TERVANIX_SRCS} INCLUDE_DIRS "." "../include" REQUIRES ${TERVANIX_REQUIRES})
