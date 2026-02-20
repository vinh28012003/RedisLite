#include "command.hpp"
#include "resp_parser.hpp"
#include <algorithm>

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

    return "-ERR unknown command '" + args[0] + "'\r\n";
}

}
