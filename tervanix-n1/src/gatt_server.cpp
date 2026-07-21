#include "gatt_server.hpp"

#include <cstdint>
#include <cstring>

#include "esp_log.h"

#include "host/ble_att.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"

#include "os/os_mbuf.h"

#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

namespace {

constexpr char TAG[] = "gatt";
constexpr char DEVICE_NAME[] = "Tervanix-Node";
constexpr char HELLO_VALUE[] = "hello-from-tervanix";

/*
 * Service:
 * 7db10000-6e8d-4d8d-a991-2cf812340000
 *
 * BLE_UUID128_INIT uses little-endian byte order.
 */
const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x34, 0x12,
    0xf8, 0x2c,
    0x91, 0xa9,
    0x8d, 0x4d,
    0x8d, 0x6e,
    0x00, 0x00, 0xb1, 0x7d
);

/*
 * Characteristic:
 * 7db10001-6e8d-4d8d-a991-2cf812340000
 */
const ble_uuid128_t hello_characteristic_uuid =
    BLE_UUID128_INIT(
        0x00, 0x00, 0x34, 0x12,
        0xf8, 0x2c,
        0x91, 0xa9,
        0x8d, 0x4d,
        0x8d, 0x6e,
        0x01, 0x00, 0xb1, 0x7d
    );

ble_gatt_chr_def characteristics[2] {};
ble_gatt_svc_def services[2] {};

int on_hello_read(
    std::uint16_t connection_handle,
    std::uint16_t attribute_handle,
    ble_gatt_access_ctxt* context,
    void* argument)
{
    (void)attribute_handle;
    (void)argument;

    if (context->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const int result = os_mbuf_append(
        context->om,
        HELLO_VALUE,
        std::strlen(HELLO_VALUE)
    );

    if (result != 0) {
        ESP_LOGE(TAG, "Failed to append characteristic value");
        return BLE_ATT_ERR_INSUFFICIENT_RES;
    }

    ESP_LOGI(
        TAG,
        "Hello characteristic read; connection=%u",
        static_cast<unsigned>(connection_handle)
    );

    return 0;
}

}  // namespace

namespace tervanix::ble {

    int initialize_gatt_server()
    {
        ESP_LOGI(TAG, "Initializing GATT server");

        ble_svc_gap_init();
        ble_svc_gatt_init();

        int result =
            ble_svc_gap_device_name_set(DEVICE_NAME);

        if (result != 0) {
            ESP_LOGE(
                TAG,
                "Failed to set device name: %d",
                result
            );
            return result;
        }

        // One readable characteristic plus the terminating empty entry.
        characteristics[0].uuid =
            &hello_characteristic_uuid.u;

        characteristics[0].access_cb =
            on_hello_read;

        characteristics[0].flags =
            BLE_GATT_CHR_F_READ;

        // One primary service plus the terminating empty entry.
        services[0].type =
            BLE_GATT_SVC_TYPE_PRIMARY;

        services[0].uuid =
            &service_uuid.u;

        services[0].characteristics =
            characteristics;

        result = ble_gatts_count_cfg(services);

        if (result != 0) {
            ESP_LOGE(
                TAG,
                "ble_gatts_count_cfg failed: %d",
                result
            );
            return result;
        }

        result = ble_gatts_add_svcs(services);

        if (result != 0) {
            ESP_LOGE(
                TAG,
                "ble_gatts_add_svcs failed: %d",
                result
            );
            return result;
        }

        ESP_LOGI(TAG, "GATT server initialized");

        return 0;
    }

}  // namespace tervanix::ble