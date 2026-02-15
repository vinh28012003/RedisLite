#include "command.hpp"
#include "resp_parser.hpp"
#include <algorithm>

namespace command {

// Convert string to uppercase for case-insensitive matching.
static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

std::string execute(const std::vector<std::string>& args, Store &store) {
    if (args.empty()) return "-ERR no command\r\n";

    std::string cmd = to_upper(args[0]);

    if(cmd == "PING") {
        return "+PONG\r\n";
    }

    if (cmd == "ECHO") {
        if (args.size() < 2) return "-ERR wrong number of arguments for 'echo' command\r\n";
        return resp::encode_bulk_string(args[1]);
    }

    if (cmd == "SET") {
        if (args.size() < 3) return "-ERR wrong number of arguments for 'set' command\r\n";
        store.set(args[1], args[2]);
        return "+OK\r\n";
    }

    if (cmd == "GET") {
        if (args.size() < 2) return "-ERR wrong number of arguments for 'get' command\r\n";
        auto value = store.get(args[1]);
        if (!value) return resp::encode_null_bulk_string();
        return resp::encode_bulk_string(*value);
    }

    return "-ERR unknown command '" + args[0] + "'\r\n";
}

}
