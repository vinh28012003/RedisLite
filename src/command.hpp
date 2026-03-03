#pragma once

#include <string>
#include <vector>
#include "store.hpp"
#include "replication_info.hpp"

namespace command {

    // Main dispatch — now takes replication metadata for INFO command
    std::string execute(const std::vector<std::string> &args, Store &store, const ReplicationInfo &repl_info);
    bool is_write_command(const std::string& cmd);
    std::string to_upper(std::string s);
}

