
#include <iostream>
#include <stdexcept>
#include "server.hpp"

int main() {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  try {
      Server server(6379);
      server.run();
  } catch (const std::runtime_error& e) {
      std::cerr << e.what() << "\n";
      return 1;
  }

  return 0;
}