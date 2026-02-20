                                        
#pragma once
                                                                                                        
#include <string>
#include <optional>
#include <utility>
#include "store.hpp"                                                                                    
#include "replication_info.hpp"

struct Client;

class Server {
    Client* listener_; // server socket wrapped as Client
    int epoll_fd_;
    Store store_;
    ReplicationInfo repl_info_;
    std::optional<std::pair<std::string, int>> replicaof_;
    int master_fd_ = -1;

    static constexpr int MAX_EVENTS = 64;

    static void set_nonblocking(int fd);
    void handle_accept();
    void handle_read(Client* client);
    void handle_write(Client* client);
    void try_send(Client* client);
    void remove_client(Client*client);
    void modify_epoll(Client* client, uint32_t events);

public:
    explicit Server(int port, const ReplicationInfo& repl_info, std::optional<std::pair<std::string, int>> replicaof = std::nullopt);
    ~Server();
    void run();

private:
    void send_and_expect(int fd, const std::string& message, const std::string& expected);
    void connect_to_master(int listen_port);
};


