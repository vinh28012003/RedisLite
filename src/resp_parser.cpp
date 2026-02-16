#include "resp_parser.hpp"
#include <stdexcept>

namespace resp {
    
ParseResult parse(const char* buffer, size_t length) {
    ParseResult result;
    size_t pos = 0;

    if (pos >= length || buffer[pos] != '*') return result;
    pos++;

    size_t count_start = pos;
    while (pos < length && buffer[pos] != '\r') pos++;
    if (pos + 1 >= length) return result;
    int count = std::stoi(std::string(buffer + count_start, pos - count_start));
    pos += 2;

    std::vector<std::string> args;  // local — not in result yet

    for (int i = 0; i < count; i++) {
        if (pos >= length || buffer[pos] != '$') return result;
        pos++;

        size_t len_start = pos;
        while (pos < length && buffer[pos] != '\r') pos++;
        if (pos + 1 >= length) return result;
        int str_len = std::stoi(std::string(buffer + len_start, pos - len_start));
        pos += 2;

        if (pos + str_len + 2 > length) return result;

        args.emplace_back(buffer + pos, str_len);
        pos += str_len + 2;
    }

    // Only assigned on complete parse
    result.args = std::move(args);
    result.bytes_consumed = pos;
    return result;
}

std::string encode_bulk_string(const std::string& str) {
    return "$" + std::to_string(str.size()) + "\r\n" + str + "\r\n";
}

std::string encode_null_bulk_string() { return "$-1\r\n"; }
}