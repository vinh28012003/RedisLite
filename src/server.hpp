                                        
#pragma once
                                                                                                        
#include <string>
#include <optional>
#include <utility>
#include <chrono>  
#include "store.hpp"                                                                                    
#include "replication_info.hpp"
#include <vector>

struct Client;

// Tracks a client blocked on WAIT — resolved when enough replicas ACK or deadline expires
struct WaitState {
    Client* client;
    int num_needed;
    int64_t target_offset;
    std::chrono::steady_clock::time_point deadline;
};

class Server {
    Client* listener_; // server socket wrapped as Client
    int epoll_fd_;
    Store store_;
    ReplicationInfo repl_info_;
    std::optional<std::pair<std::string, int>> replicaof_;
    int master_fd_ = -1;
    std::vector<Client*> replicas_;
    int64_t master_repl_offset_ = 0;       // Bytes propagated to replicas (master-side)
    std::vector<WaitState> pending_waits_;  // Clients blocked on WAIT

    static constexpr int MAX_EVENTS = 64;

    static void set_nonblocking(int fd);
    void handle_accept();
    void handle_read(Client* client);
    void handle_write(Client* client);
    void try_send(Client* client);
    void remove_client(Client* client);
    void modify_epoll(Client* client, uint32_t events);
    void handle_wait(Client* client, const std::vector<std::string>& args);
    void resolve_pending_waits();
    void check_wait_timeouts();
    int count_caught_up_replicas(int64_t target_offset);

public:
    explicit Server(int port, const ReplicationInfo& repl_info, std::optional<std::pair<std::string, int>> replicaof = std::nullopt);
    ~Server();
    void run();

private:
    void send_and_expect(int fd, const std::string& message, const std::string& expected);
    void connect_to_master(int listen_port);
};


