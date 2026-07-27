#include "ble_advertiser.hpp"

#include <array>
#include <cstdint>
#include <cstring>

#include "registration_protocol.hpp"
#include "device_identity.hpp"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"

#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_hs_id.h"
#include "host/ble_uuid.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "os/os_mbuf.h"

namespace {

constexpr char tag[] = "ble-register";
constexpr char registrar_name[] = "TVX-REG";

constexpr std::uint16_t invalid_handle = 0xffff;
constexpr std::uint16_t desired_mtu = 64;

const ble_uuid128_t service_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x34, 0x12,
    0xf8, 0x2c,
    0x91, 0xa9,
    0x8d, 0x4d,
    0x8d, 0x6e,
    0x00, 0x00, 0xb2, 0x7d
);

const ble_uuid128_t registration_uuid = BLE_UUID128_INIT(
    0x00, 0x00, 0x34, 0x12,
    0xf8, 0x2c,
    0x91, 0xa9,
    0x8d, 0x4d,
    0x8d, 0x6e,
    0x01, 0x00, 0xb2, 0x7d
);

tervanix::identity::IdentityStore* identity_store = nullptr;

std::array<std::uint8_t, 6> hardware_id {};

std::uint8_t own_address_type = 0;
std::uint16_t connection_handle = invalid_handle;
std::uint16_t service_start_handle = 0;
std::uint16_t service_end_handle = 0;
std::uint16_t registration_value_handle = 0;

std::uint32_t pending_nonce = 0;

bool host_ready = false;
bool is_scanning = false;
bool is_connecting = false;
bool is_connected = false;
bool is_registered = false;

bool pending_connect = false;
ble_addr_t pending_address {};

int on_gap_event(
    ble_gap_event* event,
    void* argument
);

void start_scan();
void begin_service_discovery();
void send_registration_request();

void reset_connection_state()
{
    connection_handle = invalid_handle;
    service_start_handle = 0;
    service_end_handle = 0;
    registration_value_handle = 0;

    is_connecting = false;
    is_connected = false;
    pending_connect = false;
}

void deferred_connect_task(void*)
{
    vTaskDelay(pdMS_TO_TICKS(150));

    if (
        !pending_connect ||
        is_connecting ||
        is_connected
    ) {
        vTaskDelete(nullptr);
        return;
    }

    pending_connect = false;
    is_connecting = true;
    is_scanning = false;

    ESP_LOGI(
        tag,
        "Connecting to Raspberry Pi"
    );

    const int result = ble_gap_connect(
        own_address_type,
        &pending_address,
        30000,
        nullptr,
        on_gap_event,
        nullptr
    );

    if (result != 0) {
        ESP_LOGE(
            tag,
            "ble_gap_connect failed: %d",
            result
        );

        is_connecting = false;
        start_scan();
    }

    vTaskDelete(nullptr);
}

void print_uuid(
    const std::uint8_t* value)
{
    ESP_LOGI(
        tag,
        "UUID=%02x%02x%02x%02x-"
        "%02x%02x-%02x%02x-"
        "%02x%02x-%02x%02x%02x%02x%02x%02x",
        value[0], value[1], value[2], value[3],
        value[4], value[5],
        value[6], value[7],
        value[8], value[9],
        value[10], value[11], value[12],
        value[13], value[14], value[15]
    );
}

bool advertisement_matches(
    const ble_gap_disc_desc& discovery)
{
    ble_hs_adv_fields fields {};

    const int result = ble_hs_adv_parse_fields(
        &fields,
        discovery.data,
        discovery.length_data
    );

    if (result != 0) {
        return false;
    }

    if (
        fields.name != nullptr &&
        fields.name_len == std::strlen(registrar_name) &&
        std::memcmp(
            fields.name,
            registrar_name,
            fields.name_len
        ) == 0
    ) {
        return true;
    }

    for (
        std::uint8_t index = 0;
        index < fields.num_uuids128;
        ++index
    ) {
        if (
            ble_uuid_cmp(
                &fields.uuids128[index].u,
                &service_uuid.u
            ) == 0
        ) {
            return true;
        }
    }

    return false;
}

void request_connection(
    const ble_addr_t& address)
{
    if (
        is_connecting ||
        is_connected ||
        pending_connect
    ) {
        return;
    }

    ESP_LOGI(
        tag,
        "Registrar found; stopping scan"
    );

    pending_address = address;
    pending_connect = true;

    const int cancel_result = ble_gap_disc_cancel();

    if (
        cancel_result != 0 &&
        cancel_result != BLE_HS_EALREADY
    ) {
        ESP_LOGE(
            tag,
            "ble_gap_disc_cancel failed: %d",
            cancel_result
        );

        pending_connect = false;
        return;
    }

    is_scanning = false;

    const BaseType_t task_result =
        xTaskCreate(
            deferred_connect_task,
            "ble-connect",
            3072,
            nullptr,
            5,
            nullptr
        );

    if (task_result != pdPASS) {
        ESP_LOGE(
            tag,
            "Could not create connection task"
        );

        pending_connect = false;
        start_scan();
    }
}


int on_registration_read(
    std::uint16_t callback_connection_handle,
    const ble_gatt_error* error,
    ble_gatt_attr* attribute,
    void* argument)
{
    (void)callback_connection_handle;
    (void)argument;

    if (error->status != 0) {
        ESP_LOGE(
            tag,
            "Registration read failed: %d",
            error->status
        );
        return 0;
    }

    if (attribute == nullptr || attribute->om == nullptr) {
        ESP_LOGE(tag, "Registration response is empty");
        return 0;
    }

    const std::uint16_t length =
        OS_MBUF_PKTLEN(attribute->om);

    if (
        length !=
        sizeof(tervanix::protocol::RegistrationResponse)
    ) {
        ESP_LOGE(
            tag,
            "Wrong response length: %u",
            static_cast<unsigned>(length)
        );
        return 0;
    }

    tervanix::protocol::RegistrationResponse response {};

    const int copy_result = os_mbuf_copydata(
        attribute->om,
        0,
        sizeof(response),
        &response
    );

    if (copy_result != 0) {
        ESP_LOGE(
            tag,
            "os_mbuf_copydata failed: %d",
            copy_result
        );
        return 0;
    }

    if (response.nonce != pending_nonce) {
        ESP_LOGE(tag, "Nonce mismatch");
        return 0;
    }

    if (
        response.status !=
            tervanix::protocol::RegistrationStatus::
                ExistingUuidAccepted &&
        response.status !=
            tervanix::protocol::RegistrationStatus::
                NewUuidAssigned
    ) {
        ESP_LOGE(
            tag,
            "Registration rejected: %u",
            static_cast<unsigned>(response.status)
        );
        return 0;
    }

    tervanix::identity::NodeUuid assigned {};

    std::memcpy(
        assigned.data(),
        response.assigned_uuid,
        assigned.size()
    );

    const esp_err_t save_result =
        identity_store->save_uuid(assigned);

    if (save_result != ESP_OK) {
        ESP_LOGE(
            tag,
            "Could not save UUID: %s",
            esp_err_to_name(save_result)
        );
        return 0;
    }

    is_registered = true;

    const char* status_text =
        response.status ==
            tervanix::protocol::RegistrationStatus::
                NewUuidAssigned
            ? "New UUID assigned"
            : "Existing UUID accepted";

    ESP_LOGI(
        tag,
        "%s",
        status_text
    );

    print_uuid(assigned.data());
    return 0;
}

int on_registration_written(
    std::uint16_t callback_connection_handle,
    const ble_gatt_error* error,
    ble_gatt_attr* attribute,
    void* argument)
{
    (void)attribute;
    (void)argument;

    if (error->status != 0) {
        ESP_LOGE(
            tag,
            "Registration write failed: %d",
            error->status
        );
        return 0;
    }

    ESP_LOGI(tag, "Registration request written");

    const int result = ble_gattc_read(
        callback_connection_handle,
        registration_value_handle,
        on_registration_read,
        nullptr
    );

    if (result != 0) {
        ESP_LOGE(
            tag,
            "ble_gattc_read failed: %d",
            result
        );
    }

    return 0;
}

void send_registration_request()
{
    tervanix::protocol::RegistrationRequest request {};

    request.version =
        tervanix::protocol::registration_version;

    std::memcpy(
        request.hardware_id,
        hardware_id.data(),
        hardware_id.size()
    );

    if (identity_store->has_uuid()) {
        request.flags |=
            tervanix::protocol::flag_has_uuid;

        std::memcpy(
            request.current_uuid,
            identity_store->uuid().data(),
            identity_store->uuid().size()
        );
    }

    pending_nonce = esp_random();
    request.nonce = pending_nonce;

    ESP_LOGI(
        tag,
        "Writing registration request (%u bytes)",
        static_cast<unsigned>(sizeof(request))
    );

    const int result = ble_gattc_write_flat(
        connection_handle,
        registration_value_handle,
        &request,
        sizeof(request),
        on_registration_written,
        nullptr
    );

    if (result != 0) {
        ESP_LOGE(
            tag,
            "ble_gattc_write_flat failed: %d",
            result
        );
    }
}

int on_characteristic_discovered(
    std::uint16_t callback_connection_handle,
    const ble_gatt_error* error,
    const ble_gatt_chr* characteristic,
    void* argument)
{
    (void)callback_connection_handle;
    (void)argument;

    if (error->status == 0 && characteristic != nullptr) {
        if (
            ble_uuid_cmp(
                &characteristic->uuid.u,
                &registration_uuid.u
            ) == 0
        ) {
            registration_value_handle =
                characteristic->val_handle;

            ESP_LOGI(
                tag,
                "Registration characteristic handle=%u",
                static_cast<unsigned>(
                    registration_value_handle
                )
            );
        }

        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (registration_value_handle == 0) {
            ESP_LOGE(
                tag,
                "Registration characteristic not found"
            );
            return 0;
        }

        send_registration_request();
        return 0;
    }

    ESP_LOGE(
        tag,
        "Characteristic discovery failed: %d",
        error->status
    );
    return 0;
}

int on_service_discovered(
    std::uint16_t callback_connection_handle,
    const ble_gatt_error* error,
    const ble_gatt_svc* service,
    void* argument)
{
    (void)argument;

    if (error->status == 0 && service != nullptr) {
        service_start_handle = service->start_handle;
        service_end_handle = service->end_handle;

        ESP_LOGI(
            tag,
            "Registration service handles=%u..%u",
            static_cast<unsigned>(service_start_handle),
            static_cast<unsigned>(service_end_handle)
        );
        return 0;
    }

    if (error->status == BLE_HS_EDONE) {
        if (
            service_start_handle == 0 ||
            service_end_handle == 0
        ) {
            ESP_LOGE(tag, "Registration service not found");
            return 0;
        }

        const int result = ble_gattc_disc_all_chrs(
            callback_connection_handle,
            service_start_handle,
            service_end_handle,
            on_characteristic_discovered,
            nullptr
        );

        if (result != 0) {
            ESP_LOGE(
                tag,
                "Characteristic discovery start failed: %d",
                result
            );
        }

        return 0;
    }

    ESP_LOGE(
        tag,
        "Service discovery failed: %d",
        error->status
    );
    return 0;
}

void begin_service_discovery()
{
    ESP_LOGI(tag, "Discovering registration service");

    const int result = ble_gattc_disc_svc_by_uuid(
        connection_handle,
        &service_uuid.u,
        on_service_discovered,
        nullptr
    );

    if (result != 0) {
        ESP_LOGE(
            tag,
            "Service discovery start failed: %d",
            result
        );
    }
}

int on_mtu_exchanged(
    std::uint16_t callback_connection_handle,
    const ble_gatt_error* error,
    std::uint16_t mtu,
    void* argument)
{
    (void)callback_connection_handle;
    (void)argument;

    if (error->status != 0) {
        ESP_LOGE(
            tag,
            "MTU exchange failed: %d",
            error->status
        );
        return 0;
    }

    ESP_LOGI(
        tag,
        "MTU=%u",
        static_cast<unsigned>(mtu)
    );

    if (mtu < 31) {
        ESP_LOGE(
            tag,
            "MTU too small; need at least 31"
        );
        return 0;
    }

    begin_service_discovery();
    return 0;
}

int on_gap_event(
    ble_gap_event* event,
    void* argument)
{
    (void)argument;

    switch (event->type) {
        case BLE_GAP_EVENT_DISC:
            if (
                !is_connected &&
                !is_connecting &&
                advertisement_matches(event->disc)
            ) {
                request_connection(event->disc.addr);
            }
            return 0;

        case BLE_GAP_EVENT_DISC_COMPLETE:
            is_scanning = false;

            ESP_LOGI(
                tag,
                "Scanning stopped; reason=%d",
                event->disc_complete.reason
            );

            if (
                !pending_connect &&
                !is_connecting &&
                !is_connected
            ) {
                start_scan();
            }

            return 0;

        case BLE_GAP_EVENT_CONNECT:
            is_connecting = false;

            if (event->connect.status != 0) {
                ESP_LOGW(
                    tag,
                    "Connection failed: %d",
                    event->connect.status
                );

                reset_connection_state();
                start_scan();
                return 0;
            }

            connection_handle =
                event->connect.conn_handle;

            is_connected = true;

            ESP_LOGI(
                tag,
                "Connected; handle=%u",
                static_cast<unsigned>(connection_handle)
            );

            {
                const int result =
                    ble_gattc_exchange_mtu(
                        connection_handle,
                        on_mtu_exchanged,
                        nullptr
                    );

                if (result != 0) {
                    ESP_LOGE(
                        tag,
                        "MTU exchange start failed: %d",
                        result
                    );
                }
            }

            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGW(
                tag,
                "Disconnected; reason=%d",
                event->disconnect.reason
            );

            reset_connection_state();
            start_scan();
            return 0;

        default:
            return 0;
    }
}

void start_scan()
{
    if (
        !host_ready ||
        is_scanning ||
        is_connecting ||
        is_connected
    ) {
        return;
    }

    ble_gap_disc_params parameters {};

    parameters.passive = 0;
    parameters.filter_duplicates = 1;
    parameters.filter_policy = 0;
    parameters.limited = 0;

    const int result = ble_gap_disc(
        own_address_type,
        BLE_HS_FOREVER,
        &parameters,
        on_gap_event,
        nullptr
    );

    if (result == 0) {
        is_scanning = true;
        ESP_LOGI(tag, "Scanning for TVX-REG");
        return;
    }

    if (result != BLE_HS_EALREADY) {
        ESP_LOGE(
            tag,
            "ble_gap_disc failed: %d",
            result
        );
    }
}

void on_reset(int reason)
{
    ESP_LOGE(tag, "NimBLE reset: %d", reason);

    host_ready = false;
    is_scanning = false;
    reset_connection_state();
}

void on_sync()
{
    const int result = ble_hs_id_infer_auto(
        0,
        &own_address_type
    );

    if (result != 0) {
        ESP_LOGE(
            tag,
            "ble_hs_id_infer_auto failed: %d",
            result
        );
        return;
    }

    host_ready = true;

    ESP_LOGI(
        tag,
        "NimBLE ready; address type=%u",
        static_cast<unsigned>(own_address_type)
    );

    start_scan();
}

void host_task(void*)
{
    ESP_LOGI(tag, "NimBLE host task started");

    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

namespace tervanix::ble {

esp_err_t start_registration_client(
    identity::IdentityStore& store)
{
    identity_store = &store;

    const esp_err_t mac_result =
        tervanix::identity::get_hardware_id(hardware_id);

    if (mac_result != ESP_OK) {
        return mac_result;
    }

    ESP_LOGI(
        tag,
        "Hardware ID=%02X:%02X:%02X:%02X:%02X:%02X",
        hardware_id[0],
        hardware_id[1],
        hardware_id[2],
        hardware_id[3],
        hardware_id[4],
        hardware_id[5]
    );

    const esp_err_t init_result =
        nimble_port_init();

    if (init_result != ESP_OK) {
        ESP_LOGE(
            tag,
            "nimble_port_init failed: %s",
            esp_err_to_name(init_result)
        );

        return init_result;
    }

    const int mtu_result =
        ble_att_set_preferred_mtu(desired_mtu);

    if (mtu_result != 0) {
        ESP_LOGE(
            tag,
            "ble_att_set_preferred_mtu failed: %d",
            mtu_result
        );

        nimble_port_deinit();
        return ESP_FAIL;
    }

    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;

    nimble_port_freertos_init(host_task);

    return ESP_OK;
}

bool connected()
{
    return is_connected;
}

bool registered()
{
    return is_registered;
}

}  // namespace tervanix::ble
