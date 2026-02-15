#include "resp_parser.hpp"
#include <stdexcept>

namespace resp {
std::vector<std::string> parse(const char* buffer, size_t length) {
    std::vector<std::string> result;
    size_t pos = 0;

    // Expect '*' - RESP array
    if (pos >= length || buffer[pos] != '*') return result;
    pos++; // skip '*'

    // Read element count (e.g. "2\r\n")
    size_t count_start = pos;
    while (pos < length && buffer[pos] != '\r') pos++;
    int count = std::stoi(std::string(buffer + count_start, pos - count_start));
    pos += 2; // skip "\r\n"

    // Read each bulk string element
    for (int i = 0; i < count && pos < length; i++)  {
        //Expect '$' - bulk string
        if (buffer[pos] != '$') break;
        pos++; // skip '$'

        // Read string length (e.g. "4\r\n")
        size_t len_start = pos;
        while (pos < length && buffer[pos] != '\r') pos++;
        int str_len = std::stoi(std::string(buffer + len_start, pos - len_start));
        pos += 2; // skip "\r\n"

        // Read exactly str_len bytes
        result.emplace_back(buffer + pos, str_len);
        pos += str_len + 2; // skip data + "\r\n"
    }
    return result;
}

std::string encode_bulk_string(const std::string& str) {
    return "$" + std::to_string(str.size()) + "\r\n" + str + "\r\n";
}

std::string encode_null_bulk_string() { return "$-1\r\n"; }
}