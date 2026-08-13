#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace tervanix::identity
{
    /** @brief Stores a 16-byte node UUID. */
    using NodeUuid = std::array<std::uint8_t, 16>;
    inline constexpr std::size_t bearer_token_capacity = 64;
    using BearerToken = std::array<char, bearer_token_capacity>;

    /**
     * @brief Stores the registered device identity.
     */
    class IdentityStore {
	public:

	/**
	 * @brief Loads saved identity data from NVS.
	 * @return ESP_OK on success, otherwise an ESP-IDF error code.
	 */
	esp_err_t begin();

	/**
	 * @brief Checks if a node UUID is stored.
	 * @return true if a UUID exists, otherwise false.
	 */
	bool has_uuid() const;

	/**
	 * @brief Gets the stored node UUID.
	 * @return The stored node UUID.
	 */
	const NodeUuid &uuid() const;
	
	/**
	 * @brief Checks if a gateway UUID is stored.
	 * @return true if a gateway UUID exists, otherwise false.
	 */
	bool has_server_uuid() const;

	/**
	 * @brief Gets the stored gateway UUID.
	 * @return The stored gateway UUID.
	 */
	const NodeUuid &server_uuid() const;
	
	/**
	 * @brief Checks if there is already an available stored bearer token.
	 * @return true if the bearer token exists, otherwise false.
	 */
	bool has_bearer_token() const;
	
	/**
	 * @brief Gets the stored bearer token.
	 * @return The stored bearer token.
	 */
	const char *bearer_token() const;

	/**
	 * @brief Saves the node UUID if one is not already stored.
	 * @note
	 * UUIDs are write-once during normal operation. Re-saving the
	 * same value succeeds, but attempting to replace an existing value
	 * is rejected.
	 *
	 * @param[in] uuid UUID assigned to the device.
	 * @return ESP_OK on success, otherwise an ESP-IDF error code.
	 */
	esp_err_t save_uuid_once(const NodeUuid &uuid);
	
	/**
	 * @brief Saves the gateway UUID if one is not already stored.
	 * @param[in] uuid UUID of the gateway.
	 * @return ESP_OK on success, otherwise an ESP-IDF error code.
	 */
	esp_err_t save_server_uuid_once(const NodeUuid &uuid);

	/**
	 * @brief Saves or updates the bearer token
	 * @note
	 * The bearer token is deliberately replaceable. Re-approval
	 * can rotate the MQTT credential while node_uuid and server_uuid
	 * remain immutable.
	 * @param[in] token The new bearer token to update
	 */
	esp_err_t save_bearer_token(const char *token);

	/**
	 * @brief Removes UUID data from identity storage.
	 * @return ESP_OK on success, otherwise an ESP-IDF error code.
	 */
	esp_err_t clear_uuid();
	
	/**
	 * @brief Removes server UUID data from identity storage.
	 * @return ESP_OK on success, otherwise an ESP-IDF error code.
	 */
	esp_err_t clear_server_uuid();
	
	/**
	 * @brief Removes bearer token data from identity storage.
	 * @return ESP_OK on success, otherwise an ESP-IDF error code.
	 */
	esp_err_t clear_bearer_token();

	private:
	bool has_uuid_ = false;
	bool has_server_uuid_ = false;
	bool has_bearer_token_ = false;
	NodeUuid uuid_{};
	NodeUuid server_uuid_{};
	BearerToken bearer_token_{};
    };

} // namespace tervanix::identity
