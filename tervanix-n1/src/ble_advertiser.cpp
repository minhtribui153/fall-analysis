#include "ble_advertiser.hpp"

#include <array>
#include <atomic>
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

    const ble_uuid128_t credential_uuid = BLE_UUID128_INIT(
        0x00, 0x00, 0x34, 0x12,
        0xf8, 0x2c,
        0x91, 0xa9,
        0x8d, 0x4d,
        0x8d, 0x6e,
        0x02, 0x00, 0xb2, 0x7d
    );

    const ble_uuid128_t worker_context_uuid = BLE_UUID128_INIT(
        0x00, 0x00, 0x34, 0x12,
        0xf8, 0x2c,
        0x91, 0xa9,
        0x8d, 0x4d,
        0x8d, 0x6e,
        0x03, 0x00, 0xb2, 0x7d
    );

    const ble_uuid128_t worker_name_uuid = BLE_UUID128_INIT(
        0x00, 0x00, 0x34, 0x12,
        0xf8, 0x2c,
        0x91, 0xa9,
        0x8d, 0x4d,
        0x8d, 0x6e,
        0x04, 0x00, 0xb2, 0x7d
    );

    tervanix::identity::IdentityStore* identity_store = nullptr;

    std::array<std::uint8_t, 6> hardware_id {};

    std::uint8_t own_address_type = 0;
    std::uint16_t connection_handle = invalid_handle;
    std::uint16_t service_start_handle = 0;
    std::uint16_t service_end_handle = 0;
    std::uint16_t registration_value_handle = 0;
    std::uint16_t credential_value_handle = 0;
    std::uint16_t worker_context_value_handle = 0;
    std::uint16_t worker_name_value_handle = 0;

    std::uint32_t pending_nonce = 0;

    bool host_ready = false;
    bool is_scanning = false;
    bool is_connecting = false;
    bool is_connected = false;
    bool is_registered = false;

    bool pending_connect = false;
    ble_addr_t pending_address {};
    std::atomic_bool host_task_stopped {true};
    std::atomic_bool registration_retry_scheduled {false};

    int on_gap_event(
        ble_gap_event* event,
        void* argument
    );

    void start_scan();
    void begin_service_discovery();
    void send_registration_request();
    void read_bearer_token();
    void read_worker_context();
    void read_worker_name();
    void schedule_registration_retry();

    void reset_connection_state()
    {
        connection_handle = invalid_handle;
        service_start_handle = 0;
        service_end_handle = 0;
        registration_value_handle = 0;
        credential_value_handle = 0;
        worker_context_value_handle = 0;
        worker_name_value_handle = 0;

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


    void registration_retry_task(void*)
    {
        vTaskDelay(pdMS_TO_TICKS(3000));
        registration_retry_scheduled.store(false);

        if (is_connected && !is_registered) {
            ESP_LOGI(tag, "Retrying BLE registration / approval check");
            send_registration_request();
        }

        vTaskDelete(nullptr);
    }

    void schedule_registration_retry()
    {
        bool expected = false;
        if (!registration_retry_scheduled.compare_exchange_strong(expected, true)) {
            return;
        }

        if (xTaskCreate(
                registration_retry_task,
                "ble-reg-retry",
                3072,
                nullptr,
                4,
                nullptr) != pdPASS) {
            registration_retry_scheduled.store(false);
            ESP_LOGE(tag, "Could not create registration retry task");
        }
    }

    int on_credential_read(
        std::uint16_t callback_connection_handle,
        const ble_gatt_error* error,
        ble_gatt_attr* attribute,
        void* argument)
    {
        (void)callback_connection_handle;
        (void)argument;

        if (error->status != 0) {
            ESP_LOGE(tag, "Bearer-token read failed: %d", error->status);
            schedule_registration_retry();
            return 0;
        }

        if (attribute == nullptr || attribute->om == nullptr) {
            ESP_LOGE(tag, "Bearer-token response is empty");
            schedule_registration_retry();
            return 0;
        }

        const std::uint16_t length = OS_MBUF_PKTLEN(attribute->om);
        if (length == 0 || length >= tervanix::identity::bearer_token_capacity) {
            ESP_LOGE(
                tag,
                "Invalid bearer-token length: %u",
                static_cast<unsigned>(length)
            );
            schedule_registration_retry();
            return 0;
        }

        char token[tervanix::identity::bearer_token_capacity] {};
        const int copy_result = os_mbuf_copydata(
            attribute->om,
            0,
            length,
            token
        );
        if (copy_result != 0) {
            ESP_LOGE(tag, "Could not copy bearer token: %d", copy_result);
            schedule_registration_retry();
            return 0;
        }
        token[length] = '\0';

        const esp_err_t save_result = identity_store->save_bearer_token(token);
        if (save_result != ESP_OK) {
            ESP_LOGE(
                tag,
                "Could not persist MQTT bearer token: %s",
                esp_err_to_name(save_result)
            );
            schedule_registration_retry();
            return 0;
        }

        ESP_LOGI(tag, "Registration approved; session bearer token stored in NVS");
        read_worker_context();
        return 0;
    }

    int on_worker_name_read(
        std::uint16_t,
        const ble_gatt_error* error,
        ble_gatt_attr* attribute,
        void*)
    {
        if (error->status != 0 || attribute == nullptr || attribute->om == nullptr) {
            ESP_LOGE(tag, "Worker-name read failed: %d", error->status);
            schedule_registration_retry();
            return 0;
        }
        const std::uint16_t length = OS_MBUF_PKTLEN(attribute->om);
        if (length == 0 || length >= tervanix::identity::worker_name_capacity) {
            ESP_LOGE(tag, "Invalid worker-name length: %u", static_cast<unsigned>(length));
            schedule_registration_retry();
            return 0;
        }
        char name[tervanix::identity::worker_name_capacity] {};
        if (os_mbuf_copydata(attribute->om, 0, length, name) != 0) {
            schedule_registration_retry();
            return 0;
        }
        name[length] = '\0';
        const esp_err_t save_result = identity_store->save_worker_name(name);
        if (save_result != ESP_OK) {
            ESP_LOGE(tag, "Could not persist worker name: %s", esp_err_to_name(save_result));
            schedule_registration_retry();
            return 0;
        }
        is_registered = true;
        ESP_LOGI(
            tag,
            "Worker assignment ready: worker_id=%s worker_name=%s expires=%u",
            identity_store->worker_id(),
            identity_store->worker_name(),
            static_cast<unsigned>(identity_store->token_expires_at())
        );
        return 0;
    }

    void read_worker_name()
    {
        if (worker_name_value_handle == 0 || connection_handle == invalid_handle) {
            schedule_registration_retry();
            return;
        }
        const int result = ble_gattc_read(
            connection_handle,
            worker_name_value_handle,
            on_worker_name_read,
            nullptr
        );
        if (result != 0) {
            ESP_LOGE(tag, "Worker-name read start failed: %d", result);
            schedule_registration_retry();
        }
    }

    int on_worker_context_read(
        std::uint16_t,
        const ble_gatt_error* error,
        ble_gatt_attr* attribute,
        void*)
    {
        if (error->status != 0 || attribute == nullptr || attribute->om == nullptr) {
            ESP_LOGE(tag, "Worker-context read failed: %d", error->status);
            schedule_registration_retry();
            return 0;
        }
        const std::uint16_t length = OS_MBUF_PKTLEN(attribute->om);
        // Server wire format is packed: 1 + 12 + 16 + 4 = 33 bytes.
        if (length != 33) {
            ESP_LOGE(tag, "Invalid worker-context length: %u", static_cast<unsigned>(length));
            schedule_registration_retry();
            return 0;
        }
        std::uint8_t wire[33] {};
        if (os_mbuf_copydata(attribute->om, 0, length, wire) != 0 || wire[0] != 1) {
            schedule_registration_retry();
            return 0;
        }
        char worker_id[13] {};
        std::memcpy(worker_id, wire + 1, 12);
        tervanix::identity::SessionUuid session_id {};
        std::memcpy(session_id.data(), wire + 13, 16);
        std::uint32_t expires = 0;
        std::memcpy(&expires, wire + 29, sizeof(expires));
        const esp_err_t save_result = identity_store->save_worker_context(
            worker_id, session_id, expires
        );
        if (save_result != ESP_OK) {
            ESP_LOGE(tag, "Could not persist worker session context: %s", esp_err_to_name(save_result));
            schedule_registration_retry();
            return 0;
        }
        read_worker_name();
        return 0;
    }

    void read_worker_context()
    {
        if (worker_context_value_handle == 0 || connection_handle == invalid_handle) {
            schedule_registration_retry();
            return;
        }
        const int result = ble_gattc_read(
            connection_handle,
            worker_context_value_handle,
            on_worker_context_read,
            nullptr
        );
        if (result != 0) {
            ESP_LOGE(tag, "Worker-context read start failed: %d", result);
            schedule_registration_retry();
        }
    }

    void read_bearer_token()
    {
        if (credential_value_handle == 0 || connection_handle == invalid_handle) {
            ESP_LOGE(tag, "Bearer-token characteristic is unavailable");
            schedule_registration_retry();
            return;
        }

        ESP_LOGI(tag, "Reading approved MQTT bearer token");
        const int result = ble_gattc_read(
            connection_handle,
            credential_value_handle,
            on_credential_read,
            nullptr
        );
        if (result != 0) {
            ESP_LOGE(tag, "Bearer-token read start failed: %d", result);
            schedule_registration_retry();
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
            if (
                response.status ==
                tervanix::protocol::RegistrationStatus::ServerRejected
            ) {
                ESP_LOGW(
                    tag,
                    "Registration is not approved yet; waiting for administrator decision"
                );
                identity_store->clear_session_credentials();
                identity_store->clear_uuid();
                schedule_registration_retry();
            } else {
                ESP_LOGE(
                    tag,
                    "Registration rejected: %u",
                    static_cast<unsigned>(response.status)
                );
            }
            return 0;
        }

        tervanix::identity::NodeUuid assigned {};
        tervanix::identity::NodeUuid registrar {};

        std::memcpy(
            assigned.data(),
            response.assigned_uuid,
            assigned.size()
        );
        std::memcpy(
            registrar.data(),
            response.server_uuid,
            registrar.size()
        );

        const esp_err_t save_server_result =
            identity_store->save_server_uuid_once(registrar);
        if (save_server_result != ESP_OK) {
            ESP_LOGE(
                tag,
                "Registration server UUID changed; refusing registration: %s",
                esp_err_to_name(save_server_result)
            );
            return 0;
        }

        const esp_err_t save_result =
            identity_store->save_uuid(assigned);

        if (save_result != ESP_OK) {
            ESP_LOGE(
                tag,
                "Could not persist assigned worker UUID: %s",
                esp_err_to_name(save_result)
            );
            return 0;
        }

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

        // The immutable registration response carries only UUID identity. MQTT
        // authorization is provisioned through the separate read-only credential
        // characteristic after administrator approval.
        read_bearer_token();

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

        if (identity_store->has_server_uuid()) {
            request.flags |=
                tervanix::protocol::flag_has_server_uuid;

            std::memcpy(
                request.expected_server_uuid,
                identity_store->server_uuid().data(),
                identity_store->server_uuid().size()
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
                registration_value_handle = characteristic->val_handle;
                ESP_LOGI(
                    tag,
                    "Registration characteristic handle=%u",
                    static_cast<unsigned>(registration_value_handle)
                );
            } else if (
                ble_uuid_cmp(
                    &characteristic->uuid.u,
                    &credential_uuid.u
                ) == 0
            ) {
                credential_value_handle = characteristic->val_handle;
                ESP_LOGI(
                    tag,
                    "Credential characteristic handle=%u",
                    static_cast<unsigned>(credential_value_handle)
                );
            } else if (
                ble_uuid_cmp(&characteristic->uuid.u, &worker_context_uuid.u) == 0
            ) {
                worker_context_value_handle = characteristic->val_handle;
                ESP_LOGI(tag, "Worker-context characteristic handle=%u",
                        static_cast<unsigned>(worker_context_value_handle));
            } else if (
                ble_uuid_cmp(&characteristic->uuid.u, &worker_name_uuid.u) == 0
            ) {
                worker_name_value_handle = characteristic->val_handle;
                ESP_LOGI(tag, "Worker-name characteristic handle=%u",
                        static_cast<unsigned>(worker_name_value_handle));
            }

            return 0;
        }

        if (error->status == BLE_HS_EDONE) {
            if (
                registration_value_handle == 0 ||
                credential_value_handle == 0 ||
                worker_context_value_handle == 0 ||
                worker_name_value_handle == 0
            ) {
                ESP_LOGE(
                    tag,
                    "Registration/session provisioning characteristic not found"
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

        if (mtu < 47) {
            ESP_LOGE(
                tag,
                "MTU too small; need at least 47 for registration"
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
                if (!is_registered) {
                    start_scan();
                }
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

        if (!is_registered) {
            start_scan();
        }
    }

    void host_task(void*)
    {
        host_task_stopped.store(false);
        ESP_LOGI(tag, "NimBLE host task started");

        nimble_port_run();
        host_task_stopped.store(true);
        ESP_LOGI(tag, "NimBLE host task stopping");
        nimble_port_freertos_deinit();
    }

    }  // namespace

    namespace tervanix::ble {

    esp_err_t start_registration_client(
        identity::IdentityStore& store)
    {
        identity_store = &store;
        is_registered = false;
        registration_retry_scheduled.store(false);

        const esp_err_t mac_result =
            tervanix::identity::get_hardware_id(hardware_id);

        if (mac_result != ESP_OK) {
            return mac_result;
        }

        ESP_LOGI(
            tag,
            "Device ID=%02X:%02X:%02X:%02X:%02X:%02X",
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

        host_task_stopped.store(false);
        nimble_port_freertos_init(host_task);

        return ESP_OK;
    }

    esp_err_t stop_registration_client()
    {
        if (!is_registered) {
            ESP_LOGE(tag, "Refusing to stop BLE before registration completes");
            return ESP_ERR_INVALID_STATE;
        }

        // Disconnect cleanly first so BlueZ sees a normal GATT termination.
        if (is_connected && connection_handle != invalid_handle) {
            ESP_LOGI(tag, "Registration complete; disconnecting BLE link");

            const int terminate_result = ble_gap_terminate(
                connection_handle,
                BLE_ERR_REM_USER_CONN_TERM
            );

            if (terminate_result != 0 && terminate_result != BLE_HS_ENOTCONN) {
                ESP_LOGW(
                    tag,
                    "ble_gap_terminate failed while stopping BLE: %d",
                    terminate_result
                );
            }

            for (unsigned i = 0; i < 40 && is_connected; ++i) {
                vTaskDelay(pdMS_TO_TICKS(50));
            }

            if (is_connected) {
                ESP_LOGW(tag, "BLE disconnect did not complete before shutdown");
            }
        }

        is_scanning = false;
        is_connecting = false;
        pending_connect = false;

        ESP_LOGI(tag, "Stopping NimBLE host");
        const int stop_result = nimble_port_stop();
        if (stop_result != 0) {
            ESP_LOGE(tag, "nimble_port_stop failed: %d", stop_result);
            return ESP_FAIL;
        }

        // host_task() performs nimble_port_freertos_deinit() after
        // nimble_port_run() returns. Wait for that before deinitializing the port.
        for (unsigned i = 0; i < 40 && !host_task_stopped.load(); ++i) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }

        if (!host_task_stopped.load()) {
            ESP_LOGE(tag, "Timed out waiting for NimBLE host task to stop");
            return ESP_ERR_TIMEOUT;
        }

        // nimble_port_freertos_deinit() runs at the tail of host_task(). Give the
        // task scheduler one short window to finish deleting the host task before
        // deinitializing the NimBLE port itself.
        vTaskDelay(pdMS_TO_TICKS(100));

        const esp_err_t deinit_result = nimble_port_deinit();
        if (deinit_result != ESP_OK) {
            ESP_LOGE(
                tag,
                "nimble_port_deinit failed: %s",
                esp_err_to_name(deinit_result)
            );
            return deinit_result;
        }

        host_ready = false;
        reset_connection_state();

        ESP_LOGI(tag, "BLE registration stack fully stopped");
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
