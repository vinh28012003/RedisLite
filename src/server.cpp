
#include "server.hpp"
#include "resp_parser.hpp"
#include "command.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <stdexcept>

void Server::set_nonblocking(int fd) {
int flags = fcntl(fd, F_GETFL, 0);
fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

Server::Server(int port) {
// --- Create socket ---
server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
if (server_fd_ < 0) {
    throw std::runtime_error("Failed to create socket");
}

int reuse = 1;
setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

sockaddr_in addr{};
addr.sin_family = AF_INET;
addr.sin_addr.s_addr = INADDR_ANY;
addr.sin_port = htons(port);

if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(server_fd_);
    throw std::runtime_error("Failed to bind to port " + std::to_string(port));
}

if (listen(server_fd_, SOMAXCONN) < 0) {
    close(server_fd_);
    throw std::runtime_error("listen failed");
}

set_nonblocking(server_fd_);

// --- Create epoll ---
epoll_fd_ = epoll_create1(0);
if (epoll_fd_ < 0) {
    close(server_fd_);
    throw std::runtime_error("epoll_create1 failed");
}

epoll_event ev{};
ev.events = EPOLLIN;
ev.data.fd = server_fd_;
epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

std::cout << "RedisLite listening on port " << port << "\n";
}

Server::~Server() {
close(server_fd_);
close(epoll_fd_);
}

void Server::handle_accept() {
sockaddr_in client_addr{};
socklen_t client_len = sizeof(client_addr);
int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

if (client_fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    std::cerr << "accept failed\n";
    return;
}

set_nonblocking(client_fd);

epoll_event ev{};
ev.events = EPOLLIN;
ev.data.fd = client_fd;
epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev);

std::cout << "Client connected (fd=" << client_fd << ")\n";
}

void Server::handle_client(int client_fd) {
char buffer[BUFFER_SIZE];
ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);

if (bytes_read > 0) {
    auto args = resp::parse(buffer, bytes_read);
    auto response = command::execute(args, store_);
    send(client_fd, response.c_str(), response.size(), 0);
} else if (bytes_read == 0) {
    std::cout << "Client disconnected (fd=" << client_fd << ")\n";
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
} else {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    std::cerr << "recv error (fd=" << client_fd << ")\n";
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
}
}

void Server::run() {
epoll_event events[MAX_EVENTS];
while (true) {
    int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

    for (int i = 0; i < num_events; i++) {
        if (events[i].data.fd == server_fd_) {
            handle_accept();
        } else {
            handle_client(events[i].data.fd);
        }
    }

    store_.evict_expired();
}
}
