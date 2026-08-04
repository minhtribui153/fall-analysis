#pragma once
#include "esp_err.h"
#include "identity_store.hpp"
#include <array>
#include <cstddef>

namespace tervanix::registry
{
    inline constexpr std::size_t max_nodes = 32;

    class RegistryStore {
        public:
        esp_err_t begin();
        esp_err_t replace(const identity::NodeUuid *uuids, std::size_t count);
        bool contains(const identity::NodeUuid &uuid) const;
        std::size_t size() const { return count_; }

        private:
        std::array<identity::NodeUuid, max_nodes> nodes_{};
        std::size_t count_ = 0;
    };
} // namespace tervanix::registry
