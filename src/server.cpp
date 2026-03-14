#include "server.hpp"
#include "client.hpp"
#include "resp_parser.hpp"
#include "command.hpp"
#include "rdb.hpp"

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
#include <csignal>

static volatile sig_atomic_t g_running = 1;

static void signal_handler(int) {
    g_running = 0;
}


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

    // Register signal handlers for graceful shutdown
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

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
            auto result = resp::parse(client->read_buf.data(), client->read_buf.size());
            if (result.bytes_consumed == 0) break;  // incomplete, wait

            bool from_master = (client->type == ClientType::MASTER_CONN);
            auto response = command::execute(result.args, store_, repl_info_, from_master);

            if (client->type == ClientType::MASTER_CONN) {
                handle_master_read(client, result.args, response, result.bytes_consumed);
            } else if (client->type == ClientType::REPLICA) {
                handle_replica_read(client, result.args);
            } else {
                std::string cmd_name = result.args.empty() ? "" : command::to_upper(result.args[0]);

                if (cmd_name == "WAIT" && response.empty()) {
                    handle_wait(client, result.args);
                } else if (cmd_name == "REPLICAOF" && response.empty()) {
                    handle_replicaof(client, result.args);
                    client->read_buf.erase(0, result.bytes_consumed);
                    continue;
                } else if (cmd_name == "PSYNC" && response.empty()) {
                    handle_psync(client, result.args);
                } else {
                    client->write_buf += response;
                }

                // Track offset + feed backlog for all writes (even with no replicas)
                if (command::is_write_command(cmd_name)) {
                    std::string propagated = resp::encode_array(result.args);
                    int64_t pre_offset = master_repl_offset_;
                    master_repl_offset_ += static_cast<int64_t>(propagated.size());
                    repl_info_.master_repl_offset = master_repl_offset_;
                    backlog_.feed(propagated, pre_offset);
                    for (auto* replica : replicas_) {
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
        ssize_t sent = send(client->fd, client->write_buf.data(), client->write_buf.size(), 0);
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
    // 3. PING
    auto ping_resp = send_and_recv(fd, resp::encode_array({"PING"}));
    if (ping_resp.find("+PONG") == std::string::npos) {
        close(fd);
        throw std::runtime_error("Expected +PONG from master");
    }

    // 4. REPLCONF listening-port
    auto rc1 = send_and_recv(fd, resp::encode_array({"REPLCONF", "listening-port", std::to_string(listen_port)}));
    if (rc1.find("+OK") == std::string::npos) {
        close(fd);
        throw std::runtime_error("Expected +OK for REPLCONF listening-port");
    }

    // 5. REPLCONF capa psync2
    auto rc2 = send_and_recv(fd, resp::encode_array({"REPLCONF", "capa", "psync2"}));
    if (rc2.find("+OK") == std::string::npos) {
        close(fd);
        throw std::runtime_error("Expected +OK for REPLCONF capa");
    }

    // 6. PSYNC — send replid + offset for partial resync attempt
    std::string psync_id = repl_info_.master_replid;
    std::string psync_offset = std::to_string(repl_info_.master_repl_offset);
    if (psync_id.empty() || repl_info_.master_repl_offset == 0) {
        psync_id = "?";
        psync_offset = "-1";
    }
    auto psync_resp = send_and_recv(fd, resp::encode_array({"PSYNC", psync_id, psync_offset}));

    // 7. Handle CONTINUE (partial resync) vs FULLRESYNC
    if (psync_resp.find("+CONTINUE") != std::string::npos) {
        // Partial resync — no RDB, keep existing data
        // Any trailing data after +CONTINUE\r\n is replayed commands (handled by event loop)
        std::cout << "Partial resync accepted by master\n";
    } else if (psync_resp.find("+FULLRESYNC") != std::string::npos) {
        // Parse replid + offset: +FULLRESYNC <replid> <offset>\r\n
        auto space1 = psync_resp.find(' ');
        auto space2 = psync_resp.find(' ', space1 + 1);
        auto crlf = psync_resp.find("\r\n");
        if (space1 != std::string::npos && space2 != std::string::npos && crlf != std::string::npos) {
            repl_info_.master_replid = psync_resp.substr(space1 + 1, space2 - space1 - 1);
            repl_info_.master_repl_offset = std::stoll(psync_resp.substr(space2 + 1, crlf - space2 - 1));
        }

        // Consume RDB — may need additional recv calls
        std::string accumulated = psync_resp;
        char buf[4096];
        while (true) {
            auto dollar = accumulated.find('$');
            if (dollar == std::string::npos) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) { close(fd); throw std::runtime_error("Failed to recv RDB from master"); }
                accumulated.append(buf, n);
                continue;
            }
            auto rdb_header_end = accumulated.find("\r\n", dollar);
            if (rdb_header_end == std::string::npos) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) { close(fd); throw std::runtime_error("Failed to recv RDB from master"); }
                accumulated.append(buf, n);
                continue;
            }
            int rdb_len;
            try {
                rdb_len = std::stoi(accumulated.substr(dollar + 1, rdb_header_end - dollar - 1));
            } catch (const std::exception&) {
                close(fd);
                throw std::runtime_error("Malformed RDB length from master");
            }
            size_t rdb_start = rdb_header_end + 2;
            while (accumulated.size() < rdb_start + rdb_len) {
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) { close(fd); throw std::runtime_error("Failed to recv RDB from master"); }
                accumulated.append(buf, n);
            }
            // Parse RDB and load into store (replaces previous discard)
            std::string rdb_data(accumulated, rdb_start, rdb_len);
            try {
                rdb::load(rdb_data, store_);
                std::cout << "RDB loaded: " << store_.size() << " keys\n";
            } catch (const std::runtime_error& e) {
                std::cerr << "RDB load failed: " << e.what() << ", continuing with empty store\n";
                store_.clear();
            }
            break;
        }
    } else {
        close(fd);
        throw std::runtime_error("Unexpected PSYNC response from master");
    }
    
    master_fd_ = fd;
    
    // 8. Register master_fd_ with epoll as MASTER_CONN
    set_nonblocking(fd);
    master_client_ = new Client(fd, ClientType::MASTER_CONN);
    epoll_event mev{};
    mev.events = EPOLLIN;
    mev.data.ptr = master_client_;
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &mev);
    
    std::cout << "Connected to master " << host << ":" << port << "\n";
}

std::string Server::send_and_recv(int fd, const std::string& message) {
    if (send(fd, message.data(), message.size(), 0) < 0) {
        close(fd);
        throw std::runtime_error("Failed to send to master");
    }

    char buf[4096];
    ssize_t n = recv(fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        close(fd);
        throw std::runtime_error("No response from master");
    }
    return std::string(buf, n);
}

// Replica-side: track replication stream bytes, respond to GETACK
void Server::handle_master_read(Client* client, const std::vector<std::string>& args,
                                const std::string& response, size_t bytes_consumed) {
    repl_info_.master_repl_offset += bytes_consumed;

    if (args.size() >= 2 && command::to_upper(args[0]) == "REPLCONF"
        && command::to_upper(args[1]) == "GETACK") {
        client->write_buf += response;
    }
}

// Master-side: parse REPLCONF ACK from replica, update offset
void Server::handle_replica_read(Client* client, const std::vector<std::string>& args) {
    if (args.size() >= 3 && command::to_upper(args[0]) == "REPLCONF"
        && command::to_upper(args[1]) == "ACK") {
        try {
            client->ack_offset = std::stoll(args[2]);
        } catch (const std::exception&) {
            return;  // Malformed ACK — ignore, don't crash
        }
        resolve_pending_waits();
    }
}

void Server::handle_psync(Client* client, const std::vector<std::string>& args) {
    std::string id = (args.size() >= 2) ? args[1] : "?";
    int64_t offset = -1;
    if (args.size() >= 3 && args[2] != "-1") {
        try { offset = std::stoll(args[2]); }
        catch (...) { offset = -1; }
    }

    // Partial resync: id matches + offset within backlog
    if (id != "?" && offset >= 0) {
        bool id_match = (id == repl_info_.master_replid) || (id == repl_info_.master_replid2 && offset <= repl_info_.second_repl_offset);

        if (id_match && backlog_.contains(offset)) {
            client->write_buf += "+CONTINUE " + repl_info_.master_replid + "\r\n";
            std::string replay = backlog_.read(offset);
            if (!replay.empty()) client->write_buf += replay;

            client->type = ClientType::REPLICA;
            replicas_.push_back(client);
            return;
        }
    }

    // Full resync: fresh replica, id mismatch, or offset outside backlog
    std::string rdb_bytes = rdb::serialize(store_);
    std::string response = "+FULLRESYNC " + repl_info_.master_replid + " " + std::to_string(master_repl_offset_) + "\r\n";
    response += "$" + std::to_string(rdb_bytes.size()) + "\r\n";
    response += rdb_bytes;
    client->write_buf += response;

    client->type = ClientType::REPLICA;
    replicas_.push_back(client);
}

// Disconnect all tracked replicas (for role change to replica)
void Server::disconnect_all_replicas() {
    auto replicas_copy = replicas_;
    for (auto* replica : replicas_copy) {
        remove_client(replica);
    }
}

// Dispatch REPLICAOF — route NO ONE vs host/port
void Server::handle_replicaof(Client* client, const std::vector<std::string>& args) {
    if (args.size() < 3) return;  // Defense-in-depth (command layer already validates)
    std::string arg1 = command::to_upper(args[1]);
    std::string arg2 = command::to_upper(args[2]);
    if (arg1 == "NO" && arg2 == "ONE") {
        handle_replicaof_no_one(client);
    } else {
        int port;
        try {
            port = std::stoi(args[2]);
        } catch (const std::exception&) {
            client->write_buf += "-ERR Invalid master port\r\n";
            try_send(client);
            return;
        }
        handle_replicaof_set_master(client, args[1], port);
    }
}

// REPLICAOF NO ONE — promote replica to master
void Server::handle_replicaof_no_one(Client* client) {
    if (repl_info_.role == "master") {
        client->write_buf += "+OK\r\n";
        try_send(client);
        return;
    }

    // Tear down master connection
    if (master_client_) {
        remove_client(master_client_);
        master_client_ = nullptr;
        master_fd_ = -1;
    }

    // Shift replication IDs: current → secondary, generate new primary
    repl_info_.master_replid2 = repl_info_.master_replid;
    repl_info_.second_repl_offset = repl_info_.master_repl_offset;
    repl_info_.master_replid = generate_replid();

    // Flip role to master, preserving offset continuity for partial resync
    repl_info_.role = "master";
    repl_info_.master_repl_offset = repl_info_.second_repl_offset;
    repl_info_.master_host = "";
    repl_info_.master_port = 0;
    replicaof_ = std::nullopt;

    // Initialize offset + backlog so siblings can partial-resync
    master_repl_offset_ = repl_info_.second_repl_offset;
    backlog_.set_start(repl_info_.second_repl_offset);

    client->write_buf += "+OK\r\n";
    try_send(client);
    std::cout << "MASTER MODE enabled (REPLICAOF NO ONE)\n";
}

// REPLICAOF host port — become replica of new master (or switch masters)
void Server::handle_replicaof_set_master(Client* client, const std::string& host, int port) {
    // Already connected to same master — no-op
    if (replicaof_ && replicaof_->first == host && replicaof_->second == port) {
        client->write_buf += "+OK Already connected to specified master\r\n";
        try_send(client);
        return;
    }

    // Tear down: disconnect replicas if we were master
    if (repl_info_.role == "master") {
        // Resolve all pending WAITs with current count before disconnecting
        for (auto& ws : pending_waits_) {
            int caught_up = count_caught_up_replicas(ws.target_offset);
            ws.client->write_buf += ":" + std::to_string(caught_up) + "\r\n";
            try_send(ws.client);
        }
        pending_waits_.clear();
        disconnect_all_replicas();
        master_repl_offset_ = 0;
    }

    // Tear down: close old master connection if we were a replica
    if (master_client_) {
        remove_client(master_client_);
        master_client_ = nullptr;
        master_fd_ = -1;
    }

    // Update role and master info
    repl_info_.role = "worker";
    repl_info_.master_repl_offset = 0;
    repl_info_.master_host = host;
    repl_info_.master_port = port;
    replicaof_ = {host, port};

    // Send OK before blocking handshake so client gets a response
    client->write_buf += "+OK\r\n";
    try_send(client);

    // Blocking handshake with new master (reuses existing connect_to_master)
    try {
        connect_to_master(port);
    } catch (const std::runtime_error& e) {
        std::cerr << "REPLICAOF failed: " << e.what() << "\n";
        // Revert to master on failure — can't be a replica without a master
        repl_info_.role = "master";
        repl_info_.master_host = "";
        repl_info_.master_port = 0;
        replicaof_ = std::nullopt;
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
    if (args.size() < 3) return;  // Defense-in-depth (command layer already validates)
    int num_needed;
    long long timeout_ms;
    try {
        num_needed = std::stoi(args[1]);
        timeout_ms = std::stoll(args[2]);
    } catch (const std::exception&) {
        client->write_buf += "-ERR value is not an integer or out of range\r\n";
        try_send(client);
        return;
    }

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
    while (g_running) {
        int num_events = epoll_wait(epoll_fd_, events, MAX_EVENTS, 100);

        if (num_events < 0) {
            if (errno == EINTR) continue;  // Signal interrupted — just retry
            std::cerr << "epoll_wait error: " << strerror(errno) << "\n";
            break;
        }

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

    std::cout << "Shutting down...\n";
}

