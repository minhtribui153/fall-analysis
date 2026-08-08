#pragma once

#include "esp_err.h"
#include <array>
#include <cstdint>

namespace tervanix::identity
{
    /** @brief Stores a 16-byte node UUID. */
    using NodeUuid = std::array<std::uint8_t, 16>;

    /**
    * @brief Stores the registered device identity.
    */
    class IdentityStore {
        public:

        /**
        * @brief Loads saved identity data from NVS.
        *
        * @return ESP_OK on success, otherwise an ESP-IDF error code.
        */
        esp_err_t begin();

        /**
        * @brief Saves the node UUID if one is not already stored.
        *
        * @param[in] uuid UUID assigned to the device.
        *
        * @return ESP_OK on success, otherwise an ESP-IDF error code.
        */
        esp_err_t save_uuid_once(const NodeUuid &uuid);

        /**
        * @brief Saves the gateway UUID if one is not already stored.
        *
        * @param[in] uuid UUID of the gateway.
        *
        * @return ESP_OK on success, otherwise an ESP-IDF error code.
        */
        esp_err_t save_server_uuid_once(const NodeUuid &uuid);

        /**
        * @brief Removes all saved identity data from NVS.
        *
        * @return ESP_OK on success, otherwise an ESP-IDF error code.
        */
        esp_err_t clear_all();

        /**
        * @brief Checks if a node UUID is stored.
        *
        * @return true if a UUID exists, otherwise false.
        */
        bool has_uuid() const;

        /**
        * @brief Gets the stored node UUID.
        *
        * @return The stored node UUID.
        */
        const NodeUuid &uuid() const;

        /**
        * @brief Checks if a gateway UUID is stored.
        *
        * @return true if a gateway UUID exists, otherwise false.
        */
        bool has_server_uuid() const;

        /**
        * @brief Gets the stored gateway UUID.
        *
        * @return The stored gateway UUID.
        */
        const NodeUuid &server_uuid() const;

        private:
        bool has_uuid_ = false;
        bool has_server_uuid_ = false;
        NodeUuid uuid_{};
        NodeUuid server_uuid_{};
    };
} // namespace tervanix::identity
