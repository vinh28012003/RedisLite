#pragma once

#include <string>
#include <vector>
#include "store.hpp"

namespace command {

// Dispatch a parsed command and return the RESP-encoded response.
std::string execute(const std::vector<std::string> &args, Store &store);

// Distpatch a parsed command and return the RESP-encode response.
std::string execute(const std::vector<std::string>& args);

}