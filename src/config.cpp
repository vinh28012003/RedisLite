#include "config.hpp"
#include <string>
#include <stdexcept>

int parse_port(int argc, char* argv[]) {
    int port = 6379;  // Redis default

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--port") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--port requires a value");
            }

            // Catch non-numeric input: stoi throws invalid_argument or out_of_range
            // Both inherit from logic_error, not runtime_error — so we catch and rethrow
            try {
                port = std::stoi(argv[i + 1]);
            } catch (const std::exception&) {
                throw std::runtime_error("invalid port value: " + std::string(argv[i + 1]));
            }

            // Valid TCP port range: 1-65535
            if (port < 1 || port > 65535) {
                throw std::runtime_error("port out of range (1-65535): " + std::to_string(port));
            }

            i++;  // skip port value on next iteration

        } else if (arg.starts_with("--")) {
            // Unknown flag — reject early rather than silently ignoring
            throw std::runtime_error("unknown option: " + arg);
        }
    }

    return port;
}