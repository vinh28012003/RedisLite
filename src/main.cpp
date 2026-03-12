
#include <iostream>
#include <stdexcept>
#include "server.hpp"
#include "config.hpp"

int main(int argc, char* argv[]) {
  std::cout << std::unitbuf;
  std::cerr << std::unitbuf;

  try {
    Config cfg = parse_config(argc, argv);
    ReplicationInfo repl_info{
      cfg.replicaof ? "worker" : "master",
      generate_replid(),
      0,
      cfg.replicaof ? cfg.replicaof->first : "",
      cfg.replicaof ? cfg.replicaof->second : 0
    };
    Server server(cfg.port, repl_info, cfg.replicaof);
    server.run();
  } catch (const std::runtime_error& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }

  return 0;
}