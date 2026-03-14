#pragma once
#include <string>
#include <atomic>

enum class ClientType { LISTENER, REGULAR, REPLICA, MASTER_CONN };

struct Client {
    int fd;
    ClientType type;
    std::string read_buf;
    std::string write_buf;
    int64_t ack_offset = 0;
    Client(int fd, ClientType type) : fd(fd), type(type) {}
};
