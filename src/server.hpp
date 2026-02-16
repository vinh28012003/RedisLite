                                        
#pragma once
                                                                                                        
#include <string>
#include "store.hpp"                                                                                    
                
class Server {
    int server_fd_;
    int epoll_fd_;
    Store store_;

    static constexpr int MAX_EVENTS = 64;
    static constexpr int BUFFER_SIZE = 1024;

    static void set_nonblocking(int fd);
    void handle_accept();
    void handle_client(int client_fd);

public:
    explicit Server(int port);
    ~Server();
    void run();
};