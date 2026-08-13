#pragma once
#include "esp_err.h"
#include "identity_store.hpp"
#include "registry_store.hpp"

namespace tervanix::registration
{
    esp_err_t verify_over_wifi(
        identity::IdentityStore &identity,
        registry::RegistryStore &registry,
        const char *server_ip,
        std::uint16_t server_port);
}
