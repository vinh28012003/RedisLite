#pragma once                                                                                            

// Parses --port from command line args.
// Returns default 6379 if not specified.
// Throws std::runtime_error if --port has no value.
int parse_port(int argc, char* argv[]);