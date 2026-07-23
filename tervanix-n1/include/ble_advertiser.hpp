#pragma once

#include "esp_err.h"
#include "identity_store.hpp"

namespace tervanix::ble {

esp_err_t start_registration_client(
    identity::IdentityStore& identity_store
);

bool connected();
bool registered();

}  // namespace tervanix::ble
