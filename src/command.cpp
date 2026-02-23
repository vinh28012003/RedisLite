#include "command.hpp"
#include "resp_parser.hpp"
#include <algorithm>

// Empty RDB file (88 bytes) — hardcoded for empty database.                           
// Decoded from base64: UkVESVMwMDEx+glyZWRpcy12ZXIFNy4yLjD6CnJlZGlzLWJpdHPAQPoFY3RpbWXCbQi8ZfoIdXNlZC1tZW3CsMQQAPoIYW9mLWJhc2XAAP/wbjv+wP9aog==
static const uint8_t EMPTY_RDB[] = {
    0x52, 0x45, 0x44, 0x49, 0x53, 0x30, 0x30, 0x31, 0x31,  // "REDIS0011"
    0xfa, 0x09, 0x72, 0x65, 0x64, 0x69, 0x73, 0x2d, 0x76, 0x65, 0x72, 0x05,
    0x37, 0x2e, 0x32, 0x2e, 0x30,                                            // redis-ver 7.2.0
    0xfa, 0x0a, 0x72, 0x65, 0x64, 0x69, 0x73, 0x2d, 0x62, 0x69, 0x74, 0x73,
    0xc0, 0x40,                                                              // redis-bits 64
    0xfa, 0x05, 0x63, 0x74, 0x69, 0x6d, 0x65, 0xc2, 0x6d, 0x08, 0xbc, 0x65,  // ctime
    0xfa, 0x08, 0x75, 0x73, 0x65, 0x64, 0x2d, 0x6d, 0x65, 0x6d, 0xc2, 0xb0,
    0xc4, 0x10, 0x00,                                                        // used-mem
    0xfa, 0x08, 0x61, 0x6f, 0x66, 0x2d, 0x62, 0x61, 0x73, 0x65, 0xc0, 0x00,  // aof-base
    0xff,                                                                    // EOF marker
    0xf0, 0x6e, 0x3b, 0xfe, 0xc0, 0xff, 0x5a, 0xa2                           // 8-byte checksum
};

namespace command {

// Convert string to uppercase for case-insensitive matching.
static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

std::string execute(const std::vector<std::string>& args, Store &store, const ReplicationInfo &repl_info) {
    if (args.empty()) return "-ERR no command\r\n";

    std::string cmd = to_upper(args[0]);

    if(cmd == "PING") {
        return "+PONG\r\n";
    }

    if (cmd == "ECHO") {
        if (args.size() < 2) return "-ERR wrong number of arguments for 'echo' command\r\n";
        return resp::encode_bulk_string(args[1]);
    }

    if (cmd == "INFO") {
        // If section sepcified, upper case it for case-insensitive match
        std::string section = (args.size() >= 2) ? to_upper(args[1]) : "";

        if (section.empty() || section == "REPLICATION") {
            std::string body = "role:" + repl_info.role + "\r\n" + "master_replid:" + repl_info.master_replid + "\r\n" + "master_repl_offset:" + std::to_string(repl_info.master_repl_offset);
            return resp::encode_bulk_string(body);
        }

        return resp::encode_bulk_string("");

    }

    if (cmd == "SET") {
        if (args.size() < 3) return "-ERR wrong number of arguments for 'set' command\r\n";

        std::optional<int64_t> px_millis;

        // Parse optional arguments: PX millis, EX seconds
        for (size_t i = 3; i + 1 < args.size(); i++) {
            std::string opt = to_upper(args[i]);
            
            if (opt == "PX") {
                try {
                    px_millis = std::stoll(args[i + 1]);
                } catch (const std::invalid_argument&) {
                    return "-ERR value is not an integer or out of range\r\n";
                } catch (const std::out_of_range&) {
                    return "-ERR value is not an integer or out of range\r\n";
                }
                px_millis = std::stoll(args[i + 1]);
                i++;    // skip the value
            } else if (opt == "EX") {
                try{
                    px_millis = std::stoll(args[i + 1]) * 1000; // seconds -> millis
                } catch (const std::invalid_argument&) {
                    return "-ERR value is not an integer or out of range\r\n";
                } catch (const std::out_of_range&) {
                    return "-ERR value is not an integer or out of range\r\n";
                }
                i++;
            }
        }

        store.set(args[1], args[2], px_millis);
        return "+OK\r\n";
    }

    if (cmd == "GET") {
        if (args.size() < 2) return "-ERR wrong number of arguments for 'get' command\r\n";
        auto value = store.get(args[1]);
        if (!value) return resp::encode_null_bulk_string();
        return resp::encode_bulk_string(*value);
    }

    if (cmd == "REPLCONF") {
        return "+OK\r\n";
    }

    if (cmd == "PSYNC") {
        std::string response = "+FULLRESYNC " + repl_info.master_replid + " 0\r\n";
        response += "$" + std::to_string(sizeof(EMPTY_RDB)) + "\r\n";
        response.append(reinterpret_cast<const char*>(EMPTY_RDB), sizeof(EMPTY_RDB));
        return response;
    }

    return "-ERR unknown command '" + args[0] + "'\r\n";
}

}
