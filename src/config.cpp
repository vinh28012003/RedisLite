#include "config.hpp"
#include <string>
#include <stdexcept>

Config parse_config(int argc, char* argv[]) {
    Config config;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--port") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--port requires a value");
            }

            // Catch non-numeric input: stoi throws invalid_argument or out_of_range
            // Both inherit from logic_error, not runtime_error — so we catch and rethrow
            try {
                config.port = std::stoi(argv[i + 1]); 
            } catch (const std::exception&) {
                throw std::runtime_error("invalid port value: " + std::string(argv[i + 1]));
            }

            // Valid TCP port range: 1-65535
            if (config.port < 1 || config.port > 65535) {
                throw std::runtime_error("port out of range (1-65535): " + std::to_string(config.port));
            }

            i++;  // skip port value on next iteration

        } else if (arg == "--replicaof") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--replicaof requires format: \"<host> <port>\"");
            }
            
            std::string value = argv[i + 1];

            // Find the space separating host and port: "localhost 6379"
            size_t space = value.find(' ');
            if (space == std::string::npos || space == 0) {
                throw std::runtime_error("--replicaof requires format: \"<host> <port>\"");
            }

            std::string host = value.substr(0, space);
            std::string port_str = value.substr(space + 1);

            // Reject extra spaces: "localhost 6379 extra"
            if (port_str.find(' ') != std::string::npos) {
                throw std::runtime_error("--replicaof requires format: \"<host> <port>\"");
            }

            int master_port;
            try {
                master_port = std::stoi(port_str);
            } catch (const std::exception&) {
                throw std::runtime_error("invalid master port: " + port_str);
            }

            if (master_port < 1 || master_port > 65535) {
                throw std::runtime_error("master port out of range (1-65535): " + std::to_string(master_port));
            }

            config.replicaof = {host, master_port};
            i++;
        
        } else if (arg.starts_with("--")) {
            // Unknown flag — reject early rather than silently ignoring
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    return config;
}