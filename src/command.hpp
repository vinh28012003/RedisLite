#pragma once

#include <string>
#include <vector>

namespace command {

// Distpatch a parsed command and return the RESP-encode response.
std::string execute(const std::vector<std::string>& args);

}