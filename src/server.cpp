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
#include <algorithm>

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
        connect_to_master(port);
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

            if (client->type == ClientType::MASTER_CONN) {
                // Replica-side: track replication stream bytes for ACK reporting
                repl_info_.master_repl_offset += result.bytes_consumed;

                if (result.args.size() >= 2 && command::to_upper(result.args[0]) == "REPLCONF" && command::to_upper(result.args[1]) == "GETACK") {
                    client->write_buf += response;
                }
            } else if (client->type == ClientType::REPLICA) {
                // Master-side: replica sent us data (REPLCONF ACK)
                if (result.args.size() >= 3 && command::to_upper(result.args[0]) == "REPLCONF" && command::to_upper(result.args[1]) == "ACK") {
                    client->ack_offset = client->repl_base_offset + std::stoll(result.args[2]);
                    resolve_pending_waits();
                }
            } else {
                // WAIT returns empty — server handles it
                if (!result.args.empty() && command::to_upper(result.args[0]) == "WAIT" && response.empty()) {
                    handle_wait(client, result.args);
                } else {
                    client->write_buf += response;
                }

                // Tag PSYNC sender as REPLICA and track for propagation
                if (!result.args.empty() && command::to_upper(result.args[0]) == "PSYNC") {
                    client->type = ClientType::REPLICA;
                    client->repl_base_offset = master_repl_offset_;
                    replicas_.push_back(client);
                }

                // Propagate write commands to all tracked replicas
                if (command::is_write_command(result.args[0]) && !replicas_.empty()) {
                    std::string propagated = resp::encode_array(result.args);
                    master_repl_offset_ += static_cast<int64_t>(propagated.size());
                    for (auto* replica: replicas_) {
                        replica->write_buf += propagated;
                        try_send(replica);
                    }
                }
            }   
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

void Server::connect_to_master(int listen_port) {
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
    // 3. Handshake step 1 PING and recv
    send_and_expect(fd, resp::encode_array({"PING"}), "+PONG");
    
    // 4. Handshale step 2 REPLCONF listening-port
    send_and_expect(fd, resp::encode_array({"REPLCONF", "listening-port", std::to_string(listen_port)}), "+OK");
    
    // 5. Handshake step 3: REPLCONF capa psync2
    send_and_expect(fd, resp::encode_array({"REPLCONF", "capa", "psync2"}), "+OK"); 

    // 6. Hand Shake step 4: PSYNC ? - 1 (request full resync)
    std::string psync_msg = resp::encode_array({"PSYNC", "?", "-1"});                                   
    if (send(fd, psync_msg.data(), psync_msg.size(), 0) < 0) {                                          
        close(fd);                                            
        throw std::runtime_error("Failed to send PSYNC to master");
    }

    // 7. handles both FULLRESYNC parsing AND RDB consumption
    char buf[4096];
    std::string accumulated;
    while (true) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            close(fd);
            throw std::runtime_error("Failed to recv PSYNC response from master");
        }
        accumulated.append(buf, n);

        if (accumulated.find("+FULLRESYNC") == std::string::npos) continue;

        auto dollar = accumulated.find('$');
        if (dollar == std::string::npos) continue;

        auto rdb_header_end = accumulated.find("\r\n", dollar);
        if (rdb_header_end == std::string::npos) continue;

        int rdb_len = std::stoi(accumulated.substr(dollar + 1, rdb_header_end - dollar - 1));
        size_t rdb_start = rdb_header_end + 2;

        if (accumulated.size() >= rdb_start + rdb_len) break;
    }
    
    master_fd_ = fd;
    
    // 8. Register master_fd_ with epoll as MASTER_CONN
    set_nonblocking(fd);
    auto* master_client = new Client(fd, ClientType::MASTER_CONN);
    epoll_event mev{};
    mev.events = EPOLLIN;
    mev.data.ptr = master_client;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &mev);
    
    std::cout << "Connected to master " << host << ":" << port << "\n";
}

void Server::send_and_expect(int fd, const std::string& message, const std::string& expected) {
    if (send(fd, message.data(), message.size(), 0) < 0) {
        close(fd);
        throw std::runtime_error("Failed to send to master");
    }

    char buf[256];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0 || std::string(buf, n).find(expected) == std::string::npos) {
        close(fd);
        throw std::runtime_error("Expected '" + expected + "' from master");
    }
}

void Server::remove_client(Client* client) {
    // Remove from replicas_ before close/delete to prevent dangling pointer
    replicas_.erase(std::remove(replicas_.begin(), replicas_.end(), client), replicas_.end());

    // Remove from pending_waits_ if this client was blocked on WAIT
    pending_waits_.erase(
        std::remove_if(pending_waits_.begin(), pending_waits_.end(),
            [client](const WaitState& ws) { return ws.client == client; }),
        pending_waits_.end());

    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, client->fd, nullptr);
    close(client->fd);
    delete client;
}

// Count replicas whose last ACK offset >= target
int Server::count_caught_up_replicas(int64_t target_offset) {
    int count = 0;
    for (const auto* replica : replicas_) {
        if (replica->ack_offset >= target_offset) count++;
    }
    return count;
}

// WAIT command handler — resolves immediately or parks the client
void Server::handle_wait(Client* client, const std::vector<std::string>& args) {
    int num_needed = std::stoi(args[1]);
    long long timeout_ms = std::stoll(args[2]);

    // no replicas connected → return 0
    // no commands propagated → all replicas are "caught up"
    if (replicas_.empty() || master_repl_offset_ == 0) {
        int count = (master_repl_offset_ == 0)
            ? static_cast<int>(replicas_.size())
            : 0;
        client->write_buf += ":" + std::to_string(count) + "\r\n";
        try_send(client);
        return;
    }

    // Fast path: enough replicas already ACK'd
    int caught_up = count_caught_up_replicas(master_repl_offset_);
    if (caught_up >= num_needed) {
        client->write_buf += ":" + std::to_string(caught_up) + "\r\n";
        try_send(client);
        return;
    }

    // send REPLCONF GETACK * to all replicas, then park
    std::string getack = resp::encode_array({"REPLCONF", "GETACK", "*"});
    for (auto* replica : replicas_) {
        replica->write_buf += getack;
        try_send(replica);
    }

    // Park client — event loop will resolve when ACKs arrive or deadline expires
    auto deadline = (timeout_ms > 0)
        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)
        : std::chrono::steady_clock::time_point::max();  // 0 timeout = wait forever

    pending_waits_.push_back({client, num_needed, master_repl_offset_, deadline});
}

// Check if any pending WAIT can be resolved (enough ACKs received)
void Server::resolve_pending_waits() {
    auto it = pending_waits_.begin();
    while (it != pending_waits_.end()) {
        int caught_up = count_caught_up_replicas(it->target_offset);
        if (caught_up >= it->num_needed) {
            it->client->write_buf += ":" + std::to_string(caught_up) + "\r\n";
            try_send(it->client);
            it = pending_waits_.erase(it);
        } else {
            ++it;
        }
    }
}

// Expire pending WAITs that have passed their deadline
void Server::check_wait_timeouts() {
    auto now = std::chrono::steady_clock::now();
    auto it = pending_waits_.begin();
    while (it != pending_waits_.end()) {
        if (now >= it->deadline) {
            int caught_up = count_caught_up_replicas(it->target_offset);
            it->client->write_buf += ":" + std::to_string(caught_up) + "\r\n";
            try_send(it->client);
            it = pending_waits_.erase(it);
        } else {
            ++it;
        }
    }
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

        check_wait_timeouts();  // Expire past-deadline WAITs each iteration
        store_.evict_expired();
    }
}

