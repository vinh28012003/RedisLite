
#include <iostream>
#include <stdexcept>
#include "server.hpp"
#include "config.hpp"

int main(int argc, char* argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  try {
    int port = parse_port(argc, argv);
    Server server(port);
    server.run();
  } catch (const std::runtime_error& e) {
      std::cerr << e.what() << "\n";
      return 1;
  }

  return 0;
}