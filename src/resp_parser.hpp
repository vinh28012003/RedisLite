#pragma once

#include <string>
#include <vector>

namespace resp {

    // Parse raw RESP bytes into a vector of string.
    // e.g. "*2\r\n$4\r\nECHO\r\n$3\r\nhey\r\n" -> {"ECHO", "hey"}
    std::vector<std::string> parse(const char* buffer, size_t length);

    // Encode a string as a RESP bulk string.
    // e.g. "hey" -> "$3\r\nhey\r\n"
    std::string encode_bulk_string(const std::string& str);

}