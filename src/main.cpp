#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include "resp_parser.hpp"
#include "command.hpp"

constexpr int PORT = 6379;
constexpr int MAX_EVENTS = 64;
constexpr int BUFFER_SIZE = 1024;

// Non-blocking is required for epoll level-triggered mode.
// If we block on read(), the entire event loop stalls.
void set_nonblocking(int fd) {
  int flags = fcntl(fd,F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int create_server_socket() {
  int server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Failed to create socket\n";
    return -1;
  }

  // Lets us restart immediately after crash without TIME_WAIT
  int reuse = 1;
  setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);

  if (bind(server_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    std::cerr << "Failed to bind to port " << PORT << "\n";
    close(server_fd);
    return -1;
  }

  if (listen(server_fd, SOMAXCONN) < 0) {
    std::cerr << "listen failed\n";
    close(server_fd);
    return -1;
  }

  set_nonblocking(server_fd);
  return server_fd;
}

// may connect between epoll_wait() cals.
void handle_accept(int server_fd, int epoll_fd) {

  sockaddr_in client_addr{};
  socklen_t client_len = sizeof(client_addr);
  int client_fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client_addr), &client_len);

  if (client_fd < 0) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return; 
    std::cerr << "accept failed\n";
    return;
  }

  set_nonblocking(client_fd);

  // Register client with epoll - wake us when it sends data
  epoll_event ev{};
  ev.events = EPOLLIN; // Level trigger
  ev.data.fd = client_fd;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev);

  std::cout << "Client connected (fd=" << client_fd << ")\n";

}

void handle_client(int client_fd, int epoll_fd) {
  char buffer[BUFFER_SIZE];


  ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);

  if (bytes_read > 0) {
    
    auto args = resp::parse(buffer, bytes_read);
    auto response = command::execute(args);
    send(client_fd, response.c_str(), response.size(), 0);

  } else if (bytes_read == 0) {
    // CLient disconnected gracefully
    std::cout << "Client disconnected (fd=" << client_fd << ")\n";
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
  } else {
    if (errno == EAGAIN || errno == EWOULDBLOCK) return;
    // Real error - drop client
    std::cerr << "recv error (fd=" << client_fd << ")\n";
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, nullptr);
    close(client_fd);
  }

}




int main(int argc, char **argv) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  int server_fd = create_server_socket();
  if (server_fd < 0) return 1;

  int epoll_fd = epoll_create1(0);
  if (epoll_fd < 0) {
    std::cerr << "epoll_create1 failed\n";
    close(server_fd);
    return 1;
  }

  // Watch the server socket for incoming connections
  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = server_fd;
  epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &ev);

  std::cout << "RedisLite listening on port " << PORT << "\n";

  // === THE EVENT LOOP ===
  epoll_event events[MAX_EVENTS];
  while (true) {
    // Block untill at least one fd is ready
    int num_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);

    for (int i = 0; i < num_events; i++) {
      if (events[i].data.fd == server_fd) {
        handle_accept(server_fd, epoll_fd);
      } else {
        handle_client(events[i].data.fd, epoll_fd);
      }
    }
  }

  close(server_fd);
  close(epoll_fd);
  return 0;
}
