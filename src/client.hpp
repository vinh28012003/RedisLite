#pragma once
#include <string>

enum class ClientType { LISTENER, REGULAR };

struct Client {
    int fd;
    ClientType type;
    std::string read_buf;
    std::string write_buf;

    Client(int fd, ClientType type) : fd(fd), type(type) {}
};