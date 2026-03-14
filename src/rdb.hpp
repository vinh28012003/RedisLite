#pragma once

#include "store.hpp"
#include <string>

namespace rdb {
    // Master-side: snapshot entire store into RDB v11 binary format
    std::string serialize(const Store& store);

    // Replica-side: parse RDB bytes, clear store, load key-value pairs
    void load(const std::string& data, Store& store);
}
