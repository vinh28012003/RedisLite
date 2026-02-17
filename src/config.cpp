#include "config.hpp"
#include <string>
#include <stdexcept>

int parse_port(int argc, char* argv[]) {
    int port = 6379;  // Redis default

    for (int i = 1; i < argc; i++) {                          // skip argv[0] (program name)
        if (std::string(argv[i]) == "--port") {
            if (i + 1 >= argc) {                              // --port is last arg, no value
                throw std::runtime_error("--port requires a value");
            }
            port = std::stoi(argv[i + 1]);                    // stoi throws on non-numeric
            i++;                                               // skip port value on next iteration
        }
    }

    return port;
}