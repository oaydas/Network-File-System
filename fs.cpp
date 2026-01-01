// Code for file server
#include <iostream>

#include "network.hpp"
#include "fs_server.h"

/// IMPORTANT: Network is big-endian, host is little-endian

int main(int argc, char* argv[]) {
    // handle input, no more than 2 should exist
    if (argc > 2) {
        std::cout << "Too many arguments passed to the program\n";
        std::cout << "./fs <portnum : optional>\n";
        return -1;
    }

    // get the port number
    int portnum = 0;

    if (argc == 2) {
        portnum = std::stoi(argv[1]); // assume argv[1] is a valid integer
    }
    
    // Create the network server
    Network network(portnum);

    try {
        network.start_server();
    } catch (const std::runtime_error& e) {
        std::cout << e.what() << std::endl;
        return -1;
    }

    return 0;
} // main()