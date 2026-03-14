#include "command.hpp"
#include "resp_parser.hpp"
#include <algorithm>

namespace command {

// Convert string to uppercase for case-insensitive matching.
std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), ::toupper);
    return s;
}

bool is_write_command(const std::string& cmd) {
    std::string upper = to_upper(cmd);
    return upper == "SET" || upper == "DEL";
}

std::string execute(const std::vector<std::string>& args, Store &store, const ReplicationInfo &repl_info, bool from_master) {
    if (args.empty()) return "-ERR no command\r\n";

    std::string cmd = to_upper(args[0]);

    // Reject write commands on replicas (but allow replication stream from master)
    if (!from_master && repl_info.role == "worker" && is_write_command(cmd))
        return "-READONLY You can't write against a read only replica.\r\n";

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
            std::string body = "role:" + repl_info.role
                + "\r\nmaster_replid:" + repl_info.master_replid
                + "\r\nmaster_repl_offset:" + std::to_string(repl_info.master_repl_offset)
                + "\r\nmaster_replid2:" + repl_info.master_replid2
                + "\r\nsecond_repl_offset:" + std::to_string(repl_info.second_repl_offset);
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
        
        if (px_millis && *px_millis <= 0) {
            return  "-ERR invalid expire time in 'set' command\r\n";
        }
        store.set(args[1], args[2], px_millis);
        return "+OK\r\n";
    }

    if (cmd == "DEL") {
        if (args.size() < 2) return "-ERR wrong number of arguments for 'del' command\r\n";
        int count = 0;
        for (size_t i = 1; i < args.size(); i++) {
            if (store.del(args[i])) count++;
        }
        return ":" + std::to_string(count) + "\r\n";
    }

    if (cmd == "GET") {
        if (args.size() < 2) return "-ERR wrong number of arguments for 'get' command\r\n";
        auto value = store.get(args[1]);
        if (!value) return resp::encode_null_bulk_string();
        return resp::encode_bulk_string(*value);
    }

    if (cmd == "ROLE") {
        // Wire protocol uses "master"/"slave" (not "worker") for client compatibility
        if (repl_info.role == "master") {
            // *3\r\n $6\r\nmaster\r\n :<offset>\r\n *0\r\n
            return "*3\r\n"
                   "$6\r\nmaster\r\n"
                   ":" + std::to_string(repl_info.master_repl_offset) + "\r\n"
                   "*0\r\n";
        } else {
            // *5\r\n $5\r\nslave\r\n $host\r\n :port\r\n $9\r\nconnected\r\n :offset\r\n
            return "*5\r\n"
                   "$5\r\nslave\r\n"
                   + resp::encode_bulk_string(repl_info.master_host)
                   + ":" + std::to_string(repl_info.master_port) + "\r\n"
                   "$9\r\nconnected\r\n"
                   ":" + std::to_string(repl_info.master_repl_offset) + "\r\n";
        }
    }

    if (cmd == "REPLCONF") {

        if (args.size() >= 2 && to_upper(args[1]) == "GETACK") {
            return resp::encode_array({"REPLCONF", "ACK", std::to_string(repl_info.master_repl_offset)});
        }
        return "+OK\r\n";
    }

    if (cmd == "PSYNC") {
        return "";  // Server owns PSYNC logic (identity check + backlog replay)
    }

    if (cmd == "REPLICAOF") {
        if (args.size() != 3) return "-ERR wrong number of arguments for 'replicaof' command\r\n";
        std::string arg1 = to_upper(args[1]);
        std::string arg2 = to_upper(args[2]);
        if (arg1 == "NO" && arg2 == "ONE") return "";  // server-handled
        // Validate port
        try {
            int port = std::stoi(args[2]);
            if (port < 1 || port > 65535) return "-ERR Invalid master port\r\n";
        } catch (const std::exception&) {
            return "-ERR Invalid master port\r\n";
        }
        return "";  // server-handled
    }

    // WAIT: validate args, return empty string to signal server-handled command
    if (cmd == "WAIT") {
        if (args.size() < 3) return "-ERR wrong number of arguments for 'wait' command\r\n";
        try {
            std::stoi(args[1]);  // numreplicas
            std::stoi(args[2]);  // timeout
        } catch (const std::exception&) {
            return "-ERR value is not an integer or out of range\r\n";
        }
        return "";  // Server owns WAIT logic
    }

    return "-ERR unknown command '" + args[0] + "'\r\n";
}

}
