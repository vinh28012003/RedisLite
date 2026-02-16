                                        
#pragma once
                                                                                                        
#include <string>
#include "store.hpp"                                                                                    
              
struct Client;

class Server {
    Client* listener_; // server socket wrapped as Client
    int epoll_fd_;
    Store store_;

    static constexpr int MAX_EVENTS = 64;

    static void set_nonblocking(int fd);
    void handle_accept();
    void handle_read(Client* client);
    void handle_write(Client* client);
    void try_send(Client* client);
    void remove_client(Client*client);
    void modify_epoll(Client* client, uint32_t events);

public:
    explicit Server(int port);
    ~Server();
    void run();
};