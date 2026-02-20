#include "server.hpp"
#include "client.hpp"
#include "resp_parser.hpp"
#include "command.hpp"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <netdb.h>

void Server::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void Server::modify_epoll(Client* client, uint32_t events) {
    epoll_event ev{};
    ev.events = events;
    ev.data.ptr = client;
    epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, client->fd, &ev);
}

void Server::remove_client(Client* client) {
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client->fd, nullptr);
    close(client->fd);
    delete client;
}

Server::Server(int port, const ReplicationInfo& repl_info, std::optional<std::pair<std::string, int>> replicaof) : repl_info_{repl_info}, replicaof_{std::move(replicaof)}  {
    // --- Create socket ---
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        throw std::runtime_error("Failed to create socket");
    }

    int reuse = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(server_fd);
        throw std::runtime_error("Failed to bind to port " + std::to_string(port));
    }

    if (listen(server_fd, SOMAXCONN) < 0) {
        close(server_fd);
        throw std::runtime_error("listen failed");
    }

    set_nonblocking(server_fd);

    // --- Create epoll ---
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ < 0) {
        close(server_fd);
        throw std::runtime_error("epoll_create1 failed");
    }

    // Wrap server socket as a LISTENER client
    listener_ = new Client(server_fd, ClientType::LISTENER);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = listener_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd, &ev);

    std::cout << "RedisLite listening on port " << port << "\n";

    // -- Handshake with master (if replica) --
    if (replicaof_) {
        connect_to_master();
    }
}

Server::~Server() {
    close(listener_->fd);
    delete listener_;
    close(epoll_fd_);
}

void Server::handle_accept() {
    sockaddr_in client_addr{};
    socklen_t client_len = sizeof(client_addr);
    int client_fd = accept(listener_->fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

    if (client_fd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        std::cerr << "accept failed\n";
        return;
    }

    int yes = 1;
    setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
    set_nonblocking(client_fd);

    Client* client = new Client(client_fd, ClientType::REGULAR);

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = client;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, client_fd, &ev);

    std::cout << "Client connected (fd=" << client_fd << ")\n";
}

void Server::handle_read(Client* client) {
    char buf[4096];
    ssize_t n = recv(client->fd, buf, sizeof(buf), 0);

    if (n > 0) {
        client->read_buf.append(buf, n);

        // Pipelining loop — process all complete commands
        while (true) {
            auto result = resp::parse(client->read_buf.data(),
                                    client->read_buf.size());
            if (result.bytes_consumed == 0) break;  // incomplete, wait

            auto response = command::execute(result.args, store_, repl_info_);
            client->write_buf += response;

            client->read_buf.erase(0, result.bytes_consumed);
        }

        if (!client->write_buf.empty()) {
            try_send(client);
        }
    } else if (n == 0) {
        std::cout << "Client disconnected (fd=" << client->fd << ")\n";
        remove_client(client);
    } else {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "recv error (fd=" << client->fd << ")\n";
            remove_client(client);
        }
    }
}

void Server::handle_write(Client* client) {
    try_send(client);
}

void Server::try_send(Client* client) {
    while (!client->write_buf.empty()) {
        ssize_t sent = send(client->fd,
                            client->write_buf.data(),
                            client->write_buf.size(), 0);
        if (sent > 0) {
            client->write_buf.erase(0, sent);
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            // Real send error — drop client
            remove_client(client);
            return;
        }
    }

    if (client->write_buf.empty()) {
        modify_epoll(client, EPOLLIN);           // done writing
    } else {
        modify_epoll(client, EPOLLIN | EPOLLOUT); // more to send
    }
}

void Server::connect_to_master() {
    auto& [host, port] = *replicaof_;

    // 1. Create socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("Failed to create master socket");
    }

    // 2. Resolve address + connect
    addrinfo hints{};
    hints.ai_family = AF_INET;          //IPv4
    hints.ai_socktype = SOCK_STREAM;    // TCP
    
    addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    int status = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (status != 0 || !result) {
        close(fd);
        freeaddrinfo(result);
        throw std::runtime_error("Failed to resolve master address: " + host);
    }

    // 3. Connect using first resolved address
    if (connect(fd, result->ai_addr, result->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(result);
        throw std::runtime_error("Failed to connect to master " + host + ":" + std::to_string(port));
    }
    freeaddrinfo(result);
    // 4. Send PING as RESP array
    std::string ping = "*1\r\n$4\r\nPING\r\n";
    if (send(fd, ping.data(), ping.size(), 0) < 0) {
        close(fd);
        throw std::runtime_error("Failed to send PING to master");
    }

    // 5. Receive PONG
    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0 || std::string(buf, n).find("+PONG") == std::string::npos) {
        close(fd);
        throw std::runtime_error("Master did not respond with PONG");
    }

    master_fd_ = fd;
    std::cout << "Connected to master " << host << ":" << port << "\n";
}

void Server::run() {
    epoll_event events[MAX_EVENTS];
    while (true) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

        for (int i = 0; i < num_events; i++) {
            Client* client = static_cast<Client*>(events[i].data.ptr);

            if (client->type == ClientType::LISTENER) {
                handle_accept();
            } else if (events[i].events & EPOLLIN) {
                handle_read(client);
                // handle_read may delete client — don't touch client after this
            } else if (events[i].events & EPOLLOUT) {
                handle_write(client);
            }
          
        }

        store_.evict_expired();
    }
}