// Code for file server
#pragma once

#include <iostream>
#include <cstring>
#include <string>
#include <deque>
#include <netdb.h>
#include <sstream>
#include <boost/regex.hpp>

#include "fs_param.h"

enum request_t { // request type
     FS_READBLOCK, 
     FS_WRITEBLOCK, 
     FS_CREATE, 
     FS_DELETE
};

struct request { // request info struct
    request_t type;                 
    int block;                      // what block was requsted
    std::string username;          
    std::string pathname;           
    std::string header;             // the original unparsed input
    char create_type;               // 'f' or 'd'
    std::deque<std::string> path;   // path split up
    char buf[FS_BLOCKSIZE];         // either the read data or the write data
};

/*
 * Parse the given header sent from the client and translate it into our
 * request data structure to pass to our file sever functions.
 * Accomplishes input error checking.
 *
 * parse_request returns trye on success, false on failure. 
 *
 */
bool parse_request(std::string &header, request &out);

/*
 * Split the path from the parser into a deque of elements.
 */
std::deque<std::string> split_path_ss(const std::string &path);

/*
 * Fill the user and path parts of our request object
*/
bool fill_user_and_path(const boost::smatch &m, request &out);

/*
 * Fill the block of our request object
*/
bool fill_block(const boost::smatch &m, request &out);


/* 
 * Use ssis_space to check for spaces 
 * Also checks for special characters
*/
bool has_space(std::string& s);