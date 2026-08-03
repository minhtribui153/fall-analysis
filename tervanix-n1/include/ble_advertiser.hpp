#pragma once

#include "esp_err.h"
#include "identity_store.hpp"

namespace tervanix::ble
{

    /**
     * @brief starts the BLE registration client
     * @details This function will start initializing the Bluetooth Low Energy (BLE)
     * registration client which is then used to interface with a registration
     * server (e.g. an available Raspberry Pi 5 BLE server)
     * @returns An error that can possibly happen when starting the BLE server.
     */
    esp_err_t
    start_registration_client(identity::IdentityStore &identity_store);

    bool connected();
    bool registered();

} // namespace tervanix::ble
