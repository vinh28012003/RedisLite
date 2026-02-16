#pragma once

#include <string>
#include <vector>

struct ParseResult {                                                                                    
    std::vector<std::string> args;                                                                      
    size_t bytes_consumed = 0;                                                                          
}; 

namespace resp {

    // Parse one RESP command from buffer.
    // Returns args + bytes consumed. bytes_consumed=0 means incomplete.
    ParseResult parse(const char* buffer, size_t length);

    // Encode a string as a RESP bulk string.
    // e.g. "hey" -> "$3\r\nhey\r\n"
    std::string encode_bulk_string(const std::string& str);

    // Encode a null bulk string for missing keys.
    // returns "$-1\r\n"
    std::string encode_null_bulk_string();

}