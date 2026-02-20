#pragma once                                                                                            

#include <string>
#include <optional>
#include <utility>

struct Config {
    int port = 6379;
    std::optional<std::pair<std::string, int>> replicaof;  // {host, port}
};

// Parses all CLI flags into a Config struct.
// Throws std::runtime_error on invalid input.
Config parse_config(int argc, char* argv[]);