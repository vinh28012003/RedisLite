
#pragma once

#include <string>
#include <optional>
#include <utility>
#include <chrono>
#include "store.hpp"
#include "replication_info.hpp"
#include "replication_backlog.hpp"
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <unordered_map>
#include <memory>

struct Client;

// Tracks a client blocked on WAIT — resolved when enough replicas ACK or deadline expires
struct WaitState {
    Client* client;
    int num_needed;
    int64_t target_offset;
    std::chrono::steady_clock::time_point deadline;
};

// Per-replica write queue — main thread appends, writer thread drains
struct ReplicaWriteQueue {
    std::string pending;                    // main thread appends propagated commands here
    std::string draining;                   // writer thread swaps pending here, then sends
    int fd;                                 // replica's socket fd (for writer thread)
    std::atomic<bool> removed{false};       // set by main thread before closing fd
};

class Server {
    Client* listener_; // server socket wrapped as Client
    int epoll_fd_;
    Store store_;
    ReplicationInfo repl_info_;
    std::optional<std::pair<std::string, int>> replicaof_;
    int master_fd_ = -1;
    Client* master_client_ = nullptr;  // Client wrapping master_fd_ (for cleanup)
    std::vector<Client*> replicas_;
    int64_t master_repl_offset_ = 0;       // Bytes propagated to replicas (master-side)
    ReplicationBacklog backlog_;            // Circular buffer for partial resync
    std::vector<WaitState> pending_waits_;  // Clients blocked on WAIT

    // IO thread for replica fan-out
    std::unordered_map<Client*, std::shared_ptr<ReplicaWriteQueue>> repl_queues_;
    std::mutex repl_mutex_;                 // protects repl_queues_ + pending buffers
    std::condition_variable repl_cv_;
    std::atomic<bool> repl_has_work_{false};
    std::jthread writer_thread_;

    static constexpr int MAX_EVENTS = 64;

    static void set_nonblocking(int fd);
    void handle_accept();
    void handle_read(Client* client);
    void handle_master_read(Client* client, const std::vector<std::string>& args, const std::string& response, size_t bytes_consumed);
    void handle_replica_read(Client* client, const std::vector<std::string>& args);
    void handle_write(Client* client);
    void try_send(Client* client);
    void remove_client(Client* client);
    void modify_epoll(Client* client, uint32_t events);
    void handle_wait(Client* client, const std::vector<std::string>& args);
    void resolve_pending_waits();
    void check_wait_timeouts();
    int count_caught_up_replicas(int64_t target_offset);

    // Writer thread
    void writer_loop(std::stop_token stop);
    void propagate_to_replicas(const std::string& data);
    void notify_writer();

public:
    explicit Server(int port, const ReplicationInfo& repl_info, std::optional<std::pair<std::string, int>> replicaof = std::nullopt);
    ~Server();
    void run();

private:
    std::string send_and_recv(int fd, const std::string& message);
    void connect_to_master(int listen_port);
    void handle_psync(Client* client, const std::vector<std::string>& args);
    void handle_replicaof(Client* client, const std::vector<std::string>& args);
    void handle_replicaof_no_one(Client* client);
    void handle_replicaof_set_master(Client* client, const std::string& host, int port);
    void disconnect_all_replicas();
};

